/**
 * @file
 * @brief 装载机制的实现：目录扫描、ELF 标记识别、dlopen、扁平配置转 C 数组
 *
 * 本文件没有跨调用保留的可变状态：dlopen 的句柄刻意不存（我们不 dlclose，
 * 句柄留着也没有第二个用途），其余函数都是纯读取或纯转换。
 */

#include "plugin/plugin_loader.h"

#include <dirent.h>
#include <dlfcn.h>
#include <elf.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace plugin {
namespace detail {

std::vector<std::string> ListPlugins(const std::string& dir) {
  DIR* handle = ::opendir(dir.c_str());
  if (handle == nullptr) return {};  // 没装插件、没这个目录都是常态

  std::vector<std::string> paths;
  while (const dirent* entry = ::readdir(handle)) {
    const std::string name = entry->d_name;
    if (name.size() <= 3 || name.compare(name.size() - 3, 3, ".so") != 0) {
      continue;
    }
    const std::string path = dir + "/" + name;
    struct stat info {};
    // stat 而不是 d_type：符号链接也要跟到目标，且 DT_UNKNOWN 在部分文件系统上
    // 是常态。
    if (::stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) continue;
    paths.push_back(path);
  }
  ::closedir(handle);
  std::sort(paths.begin(), paths.end());
  return paths;
}

/**
 * @brief 在已打开的文件里找标记节。
 * @param file 以 rb 打开的文件。
 * @param size 文件字节数。
 * @param why 失败原因。
 * @return 标记存在且值正确时 true。
 */
bool HasMarkerIn(std::FILE* file, uint64_t size, std::string& why) {
  Elf64_Ehdr header{};
  if (std::fread(&header, sizeof header, 1, file) != 1) {
    why = "short ELF header";
    return false;
  }
  if (std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0) {
    why = "not an ELF file";
    return false;
  }
  if (header.e_ident[EI_CLASS] != ELFCLASS64) {
    why = "not a 64-bit ELF";
    return false;
  }
  if (header.e_ident[EI_DATA] != ELFDATA2LSB) {
    why = "not a little-endian ELF";
    return false;
  }
  if (header.e_shnum == 0 || header.e_shentsize != sizeof(Elf64_Shdr)) {
    why = "no ELF section table";
    return false;
  }
  const uint64_t table_bytes =
      static_cast<uint64_t>(header.e_shnum) * sizeof(Elf64_Shdr);
  if (header.e_shoff > size || table_bytes > size - header.e_shoff) {
    why = "ELF section table out of range";
    return false;
  }

  std::vector<Elf64_Shdr> sections(header.e_shnum);
  if (::fseeko(file, static_cast<off_t>(header.e_shoff), SEEK_SET) != 0 ||
      std::fread(sections.data(), sizeof(Elf64_Shdr), sections.size(), file) !=
          sections.size()) {
    why = "cannot read ELF section table";
    return false;
  }

  const uint32_t names_index = header.e_shstrndx;
  if (names_index == SHN_UNDEF || names_index >= header.e_shnum) {
    why = "no ELF section name table";
    return false;
  }
  const Elf64_Shdr& names_section = sections[names_index];
  if (names_section.sh_size == 0 || names_section.sh_offset > size ||
      names_section.sh_size > size - names_section.sh_offset) {
    why = "ELF section name table out of range";
    return false;
  }
  std::vector<char> names(names_section.sh_size);
  if (::fseeko(file, static_cast<off_t>(names_section.sh_offset), SEEK_SET) !=
          0 ||
      std::fread(names.data(), 1, names.size(), file) != names.size()) {
    why = "cannot read ELF section names";
    return false;
  }

  for (const Elf64_Shdr& section : sections) {
    if (section.sh_name >= names.size()) continue;
    const std::string_view name(
        names.data() + section.sh_name,
        ::strnlen(names.data() + section.sh_name,
                  names.size() - section.sh_name));
    if (name != kPluginMarkerSection) continue;

    if (section.sh_size < sizeof(kPluginMarkerMagic) ||
        section.sh_offset > size ||
        section.sh_size > size - section.sh_offset) {
      why = "plugin marker section is malformed";
      return false;
    }
    char marker[sizeof(kPluginMarkerMagic)] = {};
    if (::fseeko(file, static_cast<off_t>(section.sh_offset), SEEK_SET) != 0 ||
        std::fread(marker, 1, sizeof marker, file) != sizeof marker) {
      why = "cannot read the plugin marker";
      return false;
    }
    // 字符串连结尾 NUL 一起比：节里必须是完整的 "DS5 Plugin\0"。
    if (std::memcmp(marker, kPluginMarkerMagic, sizeof marker) != 0) {
      why = "plugin marker does not match";
      return false;
    }
    return true;
  }
  why = std::string("no ") + kPluginMarkerSection + " section";
  return false;
}

bool HasMarker(const std::string& path, std::string& why) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    why = std::string("cannot open: ") + std::strerror(errno);
    return false;
  }
  struct stat info {};
  if (::fstat(::fileno(file), &info) != 0 || info.st_size < 0) {
    why = std::string("cannot stat: ") + std::strerror(errno);
    std::fclose(file);
    return false;
  }
  const bool found = HasMarkerIn(file, static_cast<uint64_t>(info.st_size), why);
  std::fclose(file);
  return found;
}

const ds5PluginDesc* Open(const std::string& path, std::string& why) {
  // 空路径必须自己挡：dlopen("") 不报错，它返回主程序的句柄，接着 dlsym 会去
  // 宿主符号表里找插件——那正是我们不提供导出符号的地方。
  if (path.empty()) {
    why = "empty path";
    return nullptr;
  }

  ::dlerror();  // 清掉可能残留的上一次失败
  void* handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) {
    const char* err = ::dlerror();
    why = err != nullptr ? err : "dlopen failed";
    return nullptr;
  }

  ::dlerror();
  void* symbol = ::dlsym(handle, kPluginEntryName);
  if (symbol == nullptr) {
    why = std::string("no '") + kPluginEntryName + "' symbol";
    // 不 dlclose：dlopen 已经跑过它的静态构造器，那些代码可能起过线程——
    // 和找到描述符之后的处理同一条理由，留一段映射比卸载安全。
    return nullptr;
  }
  return static_cast<const ds5PluginDesc*>(symbol);
}

std::vector<ds5Setting> BuildSettings(const Settings& settings,
                                      const std::string& name) {
  const std::string prefix = name + kKeySeparator;
  std::vector<ds5Setting> items;
  items.reserve(settings.size() + 1);
  // Settings 按扁平键有序，剥掉前缀后的相对顺序就是键名有序；键与值的指针
  // 都指向 map 节点里的文本，装载期间一直有效。
  for (const auto& [flat_key, value] : settings) {
    if (flat_key.rfind(prefix, 0) != 0) continue;
    items.push_back(ds5Setting{flat_key.c_str() + prefix.size(), value.c_str()});
  }
  items.push_back(ds5Setting{nullptr, nullptr});
  return items;
}

}  // namespace detail
}  // namespace plugin
