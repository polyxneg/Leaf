// Copyright 2026 Poly
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <cstring>
#include "core/file_sys/romfs_reader.h"
#include "core/leaf/layout_patch.h"

namespace Leaf::LayoutPatch {

namespace {

template <typename T>
T Read(const u8* data, std::size_t offset) {
    T value;
    std::memcpy(&value, data + offset, sizeof(T));
    return value;
}

template <typename T>
void Write(u8* data, std::size_t offset, T value) {
    std::memcpy(data + offset, &value, sizeof(T));
}

std::string Utf16Name(const u8* data, std::size_t offset, std::size_t bytes) {
    std::string name;
    for (std::size_t i = 0; i + 1 < bytes; i += 2) {
        const u16 c = Read<u16>(data, offset + i);
        if (c == 0)
            break;
        name.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    }
    return name;
}

constexpr u32 None = 0xFFFFFFFF;

} // namespace

std::unordered_map<std::string, FileEntry> ListRomFS(FileSys::RomFSReader& reader) {
    std::unordered_map<std::string, FileEntry> files;
    std::array<u32, 10> header{};
    if (reader.ReadFile(0, sizeof(header), reinterpret_cast<u8*>(header.data())) !=
        sizeof(header))
        return files;
    const u32 dir_meta_off = header[3], dir_meta_len = header[4];
    const u32 file_meta_off = header[7], file_meta_len = header[8], data_off = header[9];

    std::vector<u8> dirs(dir_meta_len), metas(file_meta_len);
    reader.ReadFile(dir_meta_off, dirs.size(), dirs.data());
    reader.ReadFile(file_meta_off, metas.size(), metas.data());

    // Iterative walk: (directory entry offset, path prefix)
    std::vector<std::pair<u32, std::string>> stack{{0, ""}};
    while (!stack.empty()) {
        const auto [dir, prefix] = stack.back();
        stack.pop_back();
        if (dir + 0x18 > dirs.size())
            continue;
        const u32 child = Read<u32>(dirs.data(), dir + 8);
        u32 file = Read<u32>(dirs.data(), dir + 12);
        while (file != None && file + 0x20 <= metas.size()) {
            const u32 sibling = Read<u32>(metas.data(), file + 4);
            const u64 offset = Read<u64>(metas.data(), file + 8);
            const u64 size = Read<u64>(metas.data(), file + 16);
            const u32 name_len = Read<u32>(metas.data(), file + 0x1C);
            files[prefix + Utf16Name(metas.data(), file + 0x20, name_len)] = {data_off + offset,
                                                                              size};
            file = sibling;
        }
        for (u32 c = child; c != None && c + 0x18 <= dirs.size();
             c = Read<u32>(dirs.data(), c + 4)) {
            const u32 name_len = Read<u32>(dirs.data(), c + 0x14);
            stack.emplace_back(c, prefix + Utf16Name(dirs.data(), c + 0x18, name_len) + "/");
        }
    }
    return files;
}

std::vector<u8> ReadRomFSFile(FileSys::RomFSReader& reader, const FileEntry& entry) {
    std::vector<u8> data(entry.size);
    reader.ReadFile(entry.offset, data.size(), data.data());
    return data;
}

std::vector<ArcFile> ListArc(const std::vector<u8>& arc) {
    std::vector<ArcFile> files;
    if (arc.size() < 0x1C || std::memcmp(arc.data(), "darc", 4) != 0)
        return files;
    const u32 table_off = Read<u32>(arc.data(), 0x10);
    if (table_off + 12 > arc.size())
        return files;
    const u32 count = Read<u32>(arc.data(), table_off + 8);
    const u32 names_off = table_off + count * 12;
    if (names_off > arc.size())
        return files;

    std::vector<std::pair<std::string, u32>> dirs; // (name, end index)
    for (u32 i = 0; i < count; i++) {
        const u32 entry = table_off + i * 12;
        const u32 raw_name = Read<u32>(arc.data(), entry);
        const u32 a = Read<u32>(arc.data(), entry + 4);
        const u32 b = Read<u32>(arc.data(), entry + 8);
        const bool is_dir = (raw_name >> 24) != 0;
        const std::string name = Utf16Name(arc.data(), names_off + (raw_name & 0xFFFFFF),
                                           arc.size() - names_off - (raw_name & 0xFFFFFF));
        while (!dirs.empty() && i >= dirs.back().second)
            dirs.pop_back();
        if (is_dir) {
            dirs.emplace_back(name, b);
            continue;
        }
        std::string path;
        for (const auto& [dir, end] : dirs) {
            if (!dir.empty() && dir != ".")
                path += dir + "/";
        }
        files.push_back({path + name, a, b});
    }
    return files;
}

std::optional<TextureInfo> ReadBclimInfo(const u8* data, u32 size) {
    if (size < 0x28 || std::memcmp(data + size - 0x28, "CLIM", 4) != 0)
        return std::nullopt;
    const u8* imag = data + size - 0x28 + 0x14;
    if (std::memcmp(imag, "imag", 4) != 0)
        return std::nullopt;
    TextureInfo info{Read<u16>(imag, 8), Read<u16>(imag, 10), Read<u32>(imag, 12),
                     Read<u32>(imag, 16)};
    if (info.data_size > size - 0x28)
        return std::nullopt;
    return info;
}

bool FillBclimWithKey(u8* data, u32 size) {
    const auto info = ReadBclimInfo(data, size);
    if (!info)
        return false;
    // ETC1 block in "individual" mode: both halves R=G=B base (15,0,15) i.e. (255,0,255),
    // codeword 0, all pixel indices 0 (+2). Decodes to (255,2,255) - within key tolerance.
    static constexpr u8 etc1_block[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0xFF};
    switch (info->format) {
    case 10: // ETC1
        for (u32 i = 0; i + 8 <= info->data_size; i += 8)
            std::memcpy(data + i, etc1_block, 8);
        return true;
    case 11: // ETC1A4: 8 bytes of 4-bit alpha followed by the colour block
        for (u32 i = 0; i + 16 <= info->data_size; i += 16) {
            std::memset(data + i, 0xFF, 8);
            std::memcpy(data + i + 8, etc1_block, 8);
        }
        return true;
    default:
        return false;
    }
}

u32 FindPane(const u8* clyt, u32 size, const std::string& name) {
    if (size < 0x14 || std::memcmp(clyt, "CLYT", 4) != 0)
        return 0;
    const u16 header_size = Read<u16>(clyt, 6);
    const u32 sections = Read<u32>(clyt, 0x10);
    u32 offset = header_size;
    for (u32 i = 0; i < sections && offset + 8 <= size; i++) {
        const u32 section_size = Read<u32>(clyt, offset + 4);
        static constexpr const char* pane_kinds[] = {"pan1", "pic1", "txt1",
                                                     "wnd1", "bnd1", "prt1"};
        for (const char* kind : pane_kinds) {
            if (std::memcmp(clyt + offset, kind, 4) == 0 && offset + 0x4C <= size) {
                char pane_name[0x11] = {};
                std::memcpy(pane_name, clyt + offset + 12, 0x10);
                if (name == pane_name)
                    return offset;
            }
        }
        if (section_size == 0)
            break;
        offset += section_size;
    }
    return 0;
}

// Pane section layout: magic(4) size(4) flags(1) origin(1) alpha(1) scale_flag(1) name(16)
// user(8) translate(3f @0x24) rotate(3f @0x30) scale(2f @0x3C) size(2f @0x44)

void SetPaneVisible(u8* clyt, u32 pane, bool visible) {
    clyt[pane + 8] = visible ? (clyt[pane + 8] | 1) : (clyt[pane + 8] & ~1);
}

void SetPaneAlpha(u8* clyt, u32 pane, u8 alpha) {
    clyt[pane + 10] = alpha;
}

void SetPaneTranslate(u8* clyt, u32 pane, float x, float y) {
    Write<float>(clyt, pane + 0x24, x);
    Write<float>(clyt, pane + 0x28, y);
}

void SetPaneScale(u8* clyt, u32 pane, float x, float y) {
    Write<float>(clyt, pane + 0x3C, x);
    Write<float>(clyt, pane + 0x40, y);
}

void SetPaneSize(u8* clyt, u32 pane, float width, float height) {
    Write<float>(clyt, pane + 0x44, width);
    Write<float>(clyt, pane + 0x48, height);
}

void SetPictureVertexColors(u8* clyt, u32 pane, u32 rgba) {
    if (std::memcmp(clyt + pane, "pic1", 4) != 0)
        return;
    for (u32 i = 0; i < 4; i++)
        Write<u32>(clyt, pane + 0x4C + i * 4, rgba);
}

std::vector<ArcEntry> ReadArcEntries(const std::vector<u8>& arc) {
    std::vector<ArcEntry> entries;
    for (const auto& file : ListArc(arc)) {
        if (file.offset + file.size > arc.size())
            continue;
        entries.push_back({file.path, std::vector<u8>(arc.begin() + file.offset,
                                                      arc.begin() + file.offset + file.size)});
    }
    return entries;
}

namespace {

std::string Upper(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

struct DirNode {
    std::map<std::string, DirNode> dirs;
    std::map<std::string, const std::vector<u8>*> files;
};

struct RawEntry {
    std::string name;
    bool is_dir;
    u32 a;
    u32 b;
    const std::vector<u8>* data;
};

void FlattenDir(const std::string& name, const DirNode& node, u32 parent,
                std::vector<RawEntry>& out) {
    const u32 index = static_cast<u32>(out.size());
    out.push_back({name, true, parent, 0, nullptr});
    std::vector<std::pair<std::string, bool>> children;
    for (const auto& [n, _] : node.dirs)
        children.emplace_back(n, true);
    for (const auto& [n, _] : node.files)
        children.emplace_back(n, false);
    std::sort(children.begin(), children.end(),
              [](const auto& l, const auto& r) { return Upper(l.first) < Upper(r.first); });
    for (const auto& [n, is_dir] : children) {
        if (is_dir) {
            FlattenDir(n, node.dirs.at(n), index, out);
        } else {
            const auto* data = node.files.at(n);
            out.push_back({n, false, 0, static_cast<u32>(data->size()), data});
        }
    }
    out[index].b = static_cast<u32>(out.size());
}

void AppendUtf16(std::vector<u8>& out, const std::string& s) {
    for (const char c : s) {
        out.push_back(static_cast<u8>(c));
        out.push_back(0);
    }
    out.push_back(0);
    out.push_back(0);
}

u32 Align(u32 value, u32 alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

} // namespace

std::vector<u8> WriteArc(const std::vector<ArcEntry>& entries) {
    DirNode root;
    for (const auto& entry : entries) {
        DirNode* node = &root;
        std::size_t start = 0;
        for (std::size_t slash; (slash = entry.path.find('/', start)) != std::string::npos;
             start = slash + 1) {
            node = &node->dirs[entry.path.substr(start, slash - start)];
        }
        node->files[entry.path.substr(start)] = &entry.data;
    }
    DirNode top;
    top.dirs["."] = root;
    std::vector<RawEntry> raw;
    FlattenDir("", top, 0, raw);

    std::vector<u8> names;
    std::vector<u32> name_offsets;
    for (const auto& e : raw) {
        name_offsets.push_back(static_cast<u32>(names.size()));
        AppendUtf16(names, e.name);
    }
    constexpr u32 TableOffset = 0x1C;
    const u32 table_length = static_cast<u32>(raw.size() * 12 + names.size());
    const u32 data_start = Align(TableOffset + table_length, 0x20);
    u32 offset = data_start;
    for (auto& e : raw) {
        if (e.is_dir)
            continue;
        offset = Align(offset, e.name.ends_with(".bclim") ? 0x80 : 0x20);
        e.a = offset;
        offset += e.b;
    }

    std::vector<u8> out(offset, 0);
    std::memcpy(out.data(), "darc", 4);
    Write<u16>(out.data(), 4, 0xFEFF);
    Write<u16>(out.data(), 6, 0x1C);
    Write<u32>(out.data(), 8, 0x01000000);
    Write<u32>(out.data(), 0x0C, static_cast<u32>(out.size()));
    Write<u32>(out.data(), 0x10, TableOffset);
    Write<u32>(out.data(), 0x14, table_length);
    Write<u32>(out.data(), 0x18, data_start);
    for (std::size_t i = 0; i < raw.size(); i++) {
        const u32 entry = TableOffset + static_cast<u32>(i * 12);
        Write<u32>(out.data(), entry, name_offsets[i] | (raw[i].is_dir ? 0x01000000 : 0));
        Write<u32>(out.data(), entry + 4, raw[i].a);
        Write<u32>(out.data(), entry + 8, raw[i].b);
        if (!raw[i].is_dir)
            std::memcpy(out.data() + raw[i].a, raw[i].data->data(), raw[i].b);
    }
    std::memcpy(out.data() + TableOffset + raw.size() * 12, names.data(), names.size());
    return out;
}

namespace {

/// A CLYT split into its header and sections so sections can be replaced or inserted.
struct Clyt {
    std::vector<u8> header;
    std::vector<std::vector<u8>> sections;

    static std::optional<Clyt> Parse(const std::vector<u8>& d) {
        if (d.size() < 0x14 || std::memcmp(d.data(), "CLYT", 4) != 0)
            return std::nullopt;
        Clyt c;
        const u16 header_size = Read<u16>(d.data(), 6);
        const u32 count = Read<u32>(d.data(), 0x10);
        c.header.assign(d.begin(), d.begin() + header_size);
        u32 offset = header_size;
        for (u32 i = 0; i < count; i++) {
            if (offset + 8 > d.size())
                return std::nullopt;
            const u32 size = Read<u32>(d.data(), offset + 4);
            if (size < 8 || offset + size > d.size())
                return std::nullopt;
            c.sections.emplace_back(d.begin() + offset, d.begin() + offset + size);
            offset += size;
        }
        return c;
    }

    std::vector<u8> Build() const {
        std::vector<u8> out = header;
        for (const auto& s : sections)
            out.insert(out.end(), s.begin(), s.end());
        Write<u32>(out.data(), 0x0C, static_cast<u32>(out.size()));
        Write<u32>(out.data(), 0x10, static_cast<u32>(sections.size()));
        return out;
    }

    int Find(const char* magic) const {
        for (std::size_t i = 0; i < sections.size(); i++) {
            if (std::memcmp(sections[i].data(), magic, 4) == 0)
                return static_cast<int>(i);
        }
        return -1;
    }

    static bool IsPane(const std::vector<u8>& s) {
        static constexpr const char* kinds[] = {"pan1", "pic1", "txt1", "wnd1", "bnd1", "prt1"};
        return s.size() >= 0x4C && std::any_of(std::begin(kinds), std::end(kinds), [&](auto k) {
                   return std::memcmp(s.data(), k, 4) == 0;
               });
    }

    static std::string PaneName(const std::vector<u8>& s) {
        char name[0x11] = {};
        std::memcpy(name, s.data() + 12, 0x10);
        return name;
    }

    int FindPane(const std::string& name) const {
        for (std::size_t i = 0; i < sections.size(); i++) {
            if (IsPane(sections[i]) && PaneName(sections[i]) == name)
                return static_cast<int>(i);
        }
        return -1;
    }
};

std::vector<std::string> ReadStringTable(const std::vector<u8>& s) {
    std::vector<std::string> out;
    const u32 count = Read<u32>(s.data(), 8);
    for (u32 i = 0; i < count; i++) {
        const u32 start = 12 + Read<u32>(s.data(), 12 + i * 4);
        out.emplace_back(reinterpret_cast<const char*>(s.data() + start));
    }
    return out;
}

std::vector<u8> BuildStringTable(const char* magic, const std::vector<std::string>& strings) {
    std::vector<u8> body(4 + strings.size() * 4);
    Write<u32>(body.data(), 0, static_cast<u32>(strings.size()));
    u32 base = static_cast<u32>(strings.size() * 4);
    std::vector<u8> blob;
    for (std::size_t i = 0; i < strings.size(); i++) {
        Write<u32>(body.data(), 4 + i * 4, base + static_cast<u32>(blob.size()));
        blob.insert(blob.end(), strings[i].begin(), strings[i].end());
        blob.push_back(0);
    }
    body.insert(body.end(), blob.begin(), blob.end());
    while ((body.size() + 8) % 4)
        body.push_back(0);
    std::vector<u8> s(8);
    std::memcpy(s.data(), magic, 4);
    Write<u32>(s.data(), 4, static_cast<u32>(body.size() + 8));
    s.insert(s.end(), body.begin(), body.end());
    return s;
}

std::vector<std::vector<u8>> ReadMaterials(const std::vector<u8>& s) {
    std::vector<std::vector<u8>> out;
    const u32 count = Read<u32>(s.data(), 8);
    for (u32 i = 0; i < count; i++) {
        const u32 start = Read<u32>(s.data(), 12 + i * 4);
        const u32 end = i + 1 < count ? Read<u32>(s.data(), 12 + (i + 1) * 4)
                                      : static_cast<u32>(s.size());
        out.emplace_back(s.begin() + start, s.begin() + end);
    }
    return out;
}

std::vector<u8> BuildMaterials(const std::vector<std::vector<u8>>& materials) {
    const u32 base = 12 + static_cast<u32>(materials.size() * 4);
    std::vector<u8> s(base);
    std::memcpy(s.data(), "mat1", 4);
    Write<u32>(s.data(), 8, static_cast<u32>(materials.size()));
    for (std::size_t i = 0; i < materials.size(); i++) {
        Write<u32>(s.data(), 12 + i * 4, static_cast<u32>(s.size()));
        s.insert(s.end(), materials[i].begin(), materials[i].end());
    }
    Write<u32>(s.data(), 4, static_cast<u32>(s.size()));
    return s;
}

ArcEntry* FindEntry(std::vector<ArcEntry>& arc, const std::string& path) {
    for (auto& e : arc) {
        if (e.path == path)
            return &e;
    }
    return nullptr;
}

const ArcEntry* FindEntry(const std::vector<ArcEntry>& arc, const std::string& path) {
    for (const auto& e : arc) {
        if (e.path == path)
            return &e;
    }
    return nullptr;
}

} // namespace

bool GraftWindow(std::vector<ArcEntry>& dst, const std::vector<ArcEntry>& src,
                 const GraftWindowParams& p) {
    ArcEntry* dst_entry = FindEntry(dst, p.dst_layout);
    const ArcEntry* src_entry = FindEntry(src, p.src_layout);
    if (!dst_entry || !src_entry)
        return false;
    auto d = Clyt::Parse(dst_entry->data);
    auto s = Clyt::Parse(src_entry->data);
    if (!d || !s)
        return false;
    const int s_txl = s->Find("txl1"), s_mat = s->Find("mat1"), s_wnd = s->FindPane(p.src_pane);
    const int d_txl = d->Find("txl1"), d_mat = d->Find("mat1"), d_before = d->FindPane(p.before);
    if (s_txl < 0 || s_mat < 0 || s_wnd < 0 || d_txl < 0 || d_mat < 0 || d_before < 0 ||
        std::memcmp(s->sections[s_wnd].data(), "wnd1", 4) != 0)
        return false;

    const auto src_textures = ReadStringTable(s->sections[s_txl]);
    const auto src_materials = ReadMaterials(s->sections[s_mat]);
    auto textures = ReadStringTable(d->sections[d_txl]);
    auto materials = ReadMaterials(d->sections[d_mat]);

    auto texture_index = [&](const std::string& name) -> u16 {
        auto it = std::find(textures.begin(), textures.end(), name);
        if (it != textures.end())
            return static_cast<u16>(it - textures.begin());
        textures.push_back(name);
        if (!FindEntry(dst, "timg/" + name)) {
            if (const ArcEntry* tex = FindEntry(src, "timg/" + name))
                dst.push_back(*tex);
        }
        return static_cast<u16>(textures.size() - 1);
    };

    bool ok = true;
    auto copy_material = [&](u16 index) -> u16 {
        if (index >= src_materials.size()) {
            ok = false;
            return 0;
        }
        std::vector<u8> m = src_materials[index];
        const u32 flags = Read<u32>(m.data(), 0x30);
        for (u32 t = 0; t < (flags & 3); t++) {
            const u32 o = 0x34 + t * 4;
            const u16 src_tex = Read<u16>(m.data(), o);
            if (src_tex >= src_textures.size()) {
                ok = false;
                continue;
            }
            Write<u16>(m.data(), o, texture_index(src_textures[src_tex]));
        }
        // Unique material name: new pane name + the suffix the original used (C, LT, RT, ...).
        char old_name[0x15] = {};
        std::memcpy(old_name, m.data(), 0x14);
        std::string suffix = old_name;
        suffix = suffix.size() > p.src_pane.size() ? suffix.substr(p.src_pane.size()) : "";
        const std::string name = (p.new_name + suffix).substr(0, 19);
        std::memset(m.data(), 0, 0x14);
        std::memcpy(m.data(), name.data(), name.size());
        materials.push_back(std::move(m));
        return static_cast<u16>(materials.size() - 1);
    };

    std::vector<u8> wnd = s->sections[s_wnd];
    const u32 content = Read<u32>(wnd.data(), 0x60);
    const u32 frame_table = Read<u32>(wnd.data(), 0x64);
    const u8 frames = wnd[0x5C];
    Write<u16>(wnd.data(), content + 16, copy_material(Read<u16>(wnd.data(), content + 16)));
    for (u8 f = 0; f < frames; f++) {
        const u32 frame = Read<u32>(wnd.data(), frame_table + f * 4);
        Write<u16>(wnd.data(), frame, copy_material(Read<u16>(wnd.data(), frame)));
    }
    if (!ok)
        return false;
    std::memset(wnd.data() + 12, 0, 0x10);
    std::memcpy(wnd.data() + 12, p.new_name.data(), std::min<std::size_t>(p.new_name.size(), 16));
    wnd[8] |= 1;
    Write<float>(wnd.data(), 0x24, p.x);
    Write<float>(wnd.data(), 0x28, p.y);
    Write<float>(wnd.data(), 0x3C, p.scale_x);
    Write<float>(wnd.data(), 0x40, p.scale_y);
    Write<float>(wnd.data(), 0x44, p.width);
    Write<float>(wnd.data(), 0x48, p.height);

    d->sections[d_txl] = BuildStringTable("txl1", textures);
    d->sections[d_mat] = BuildMaterials(materials);
    d->sections.insert(d->sections.begin() + d_before, std::move(wnd));
    dst_entry = FindEntry(dst, p.dst_layout); // dst may have grown
    dst_entry->data = d->Build();
    return true;
}

bool WrapRootChildren(std::vector<ArcEntry>& arc, const std::string& layout, float scale_x,
                      float scale_y, float pivot_x, float pivot_y) {
    ArcEntry* entry = FindEntry(arc, layout);
    if (!entry)
        return false;
    auto c = Clyt::Parse(entry->data);
    if (!c)
        return false;
    const int root = c->FindPane("RootPane");
    if (root < 0 || root + 1 >= static_cast<int>(c->sections.size()) ||
        std::memcmp(c->sections[root + 1].data(), "pas1", 4) != 0)
        return false;
    // Find the pae1 closing RootPane's children.
    int depth = 0, close = -1;
    for (int i = root + 1; i < static_cast<int>(c->sections.size()); i++) {
        if (std::memcmp(c->sections[i].data(), "pas1", 4) == 0)
            depth++;
        else if (std::memcmp(c->sections[i].data(), "pae1", 4) == 0 && --depth == 0) {
            close = i;
            break;
        }
    }
    if (close < 0)
        return false;

    const auto make_pane = [&](const char* name, float x, float y, float sx, float sy) {
        std::vector<u8> pane = c->sections[root];
        std::memset(pane.data() + 12, 0, 0x10);
        std::memcpy(pane.data() + 12, name, std::strlen(name));
        pane[8] |= 1;
        pane[10] = 255;
        Write<float>(pane.data(), 0x24, x);
        Write<float>(pane.data(), 0x28, y);
        Write<float>(pane.data(), 0x3C, sx);
        Write<float>(pane.data(), 0x40, sy);
        return pane;
    };
    const auto marker = [](const char* magic) {
        std::vector<u8> s(8);
        std::memcpy(s.data(), magic, 4);
        Write<u32>(s.data(), 4, 8);
        return s;
    };
    auto& s = c->sections;
    s.insert(s.begin() + close, {marker("pae1"), marker("pae1")});
    s.insert(s.begin() + root + 2,
             {make_pane("N_leaf_asp", pivot_x, pivot_y, scale_x, scale_y), marker("pas1"),
              make_pane("N_leaf_asp_in", -pivot_x, -pivot_y, 1.0f, 1.0f), marker("pas1")});
    entry->data = c->Build();
    return true;
}

bool WrapForAnimation(std::vector<ArcEntry>& arc, const AnimWrapParams& params) {
    ArcEntry* entry = FindEntry(arc, params.layout);
    if (!entry || params.wrapper.size() > 0x10)
        return false;
    auto c = Clyt::Parse(entry->data);
    if (!c)
        return false;
    auto& s = c->sections;
    const int first = c->FindPane(params.first);
    int last = c->FindPane(params.last);
    const int animated = c->FindPane(params.animated);
    const int root = c->FindPane("RootPane");
    if (first < 0 || last < first || animated < first || animated > last || root < 0)
        return false;
    // Include the children of the last pane.
    if (last + 1 < static_cast<int>(s.size()) && std::memcmp(s[last + 1].data(), "pas1", 4) == 0) {
        int depth = 0;
        for (int i = last + 1; i < static_cast<int>(s.size()); i++) {
            if (std::memcmp(s[i].data(), "pas1", 4) == 0)
                depth++;
            else if (std::memcmp(s[i].data(), "pae1", 4) == 0 && --depth == 0) {
                last = i;
                break;
            }
        }
    }

    // The wrapper takes the animated pane's place and transform; the wrapped panes keep their
    // positions relative to it.
    const float x = Read<float>(s[animated].data(), 0x24);
    const float y = Read<float>(s[animated].data(), 0x28);
    std::vector<u8> pane = s[root];
    std::memset(pane.data() + 12, 0, 0x10);
    std::memcpy(pane.data() + 12, params.wrapper.data(), params.wrapper.size());
    pane[8] |= 1;
    pane[10] = 255;
    std::memcpy(pane.data() + 0x24, s[animated].data() + 0x24, 0x20); // translate/rotate/scale
    Write<float>(s[animated].data(), 0x3C, 1.0f);
    Write<float>(s[animated].data(), 0x40, 1.0f);
    int depth = 0;
    for (int i = first; i <= last; i++) {
        if (std::memcmp(s[i].data(), "pas1", 4) == 0)
            depth++;
        else if (std::memcmp(s[i].data(), "pae1", 4) == 0)
            depth--;
        else if (depth == 0 && Clyt::IsPane(s[i])) {
            Write<float>(s[i].data(), 0x24, Read<float>(s[i].data(), 0x24) - x);
            Write<float>(s[i].data(), 0x28, Read<float>(s[i].data(), 0x28) - y);
        }
    }
    const auto marker = [](const char* magic) {
        std::vector<u8> m(8);
        std::memcpy(m.data(), magic, 4);
        Write<u32>(m.data(), 4, 8);
        return m;
    };
    s.insert(s.begin() + last + 1, marker("pae1"));
    s.insert(s.begin() + first, {pane, marker("pas1")});

    // Groups bind the animations: the wrapper replaces the animated pane in them.
    for (auto& sec : s) {
        if (std::memcmp(sec.data(), "grp1", 4) != 0 || sec.size() < 0x1C)
            continue;
        const u32 count = Read<u32>(sec.data(), 0x18);
        for (u32 i = 0; i < count && 0x1C + (i + 1) * 0x10 <= sec.size(); i++) {
            u8* name = sec.data() + 0x1C + i * 0x10;
            if (std::string(reinterpret_cast<char*>(name), strnlen(reinterpret_cast<char*>(name),
                                                                    0x10)) == params.animated) {
                std::memset(name, 0, 0x10);
                std::memcpy(name, params.wrapper.data(), params.wrapper.size());
            }
        }
    }
    entry->data = c->Build();

    // Retarget the animations (pai1 entries start with a 20-byte pane name).
    for (const auto& anim_path : params.anims) {
        ArcEntry* anim = FindEntry(arc, anim_path);
        if (!anim || anim->data.size() < 0x14 || std::memcmp(anim->data.data(), "CLAN", 4) != 0)
            return false;
        auto& d = anim->data;
        u32 offset = Read<u16>(d.data(), 6);
        const u32 count = Read<u32>(d.data(), 0x10);
        for (u32 n = 0; n < count && offset + 8 <= d.size(); n++) {
            const u32 size = Read<u32>(d.data(), offset + 4);
            if (std::memcmp(d.data() + offset, "pai1", 4) == 0 && offset + 0x14 <= d.size()) {
                const u32 entries = Read<u16>(d.data(), offset + 14);
                const u32 table = offset + Read<u32>(d.data(), offset + 16);
                for (u32 i = 0; i < entries && table + (i + 1) * 4 <= d.size(); i++) {
                    const u32 e = offset + Read<u32>(d.data(), table + i * 4);
                    if (e + 20 > d.size())
                        continue;
                    char* name = reinterpret_cast<char*>(d.data() + e);
                    if (std::string(name, strnlen(name, 20)) == params.animated) {
                        std::memset(name, 0, 20);
                        std::memcpy(name, params.wrapper.data(), params.wrapper.size());
                    }
                }
            }
            if (size < 8)
                break;
            offset += size;
        }
    }
    return true;
}

bool ShiftChildren(std::vector<ArcEntry>& arc, const std::string& layout,
                   const std::string& parent, float dx, float dy) {
    ArcEntry* entry = FindEntry(arc, layout);
    if (!entry)
        return false;
    auto c = Clyt::Parse(entry->data);
    if (!c)
        return false;
    auto& s = c->sections;
    const int index = c->FindPane(parent);
    if (index < 0 || index + 1 >= static_cast<int>(s.size()) ||
        std::memcmp(s[index + 1].data(), "pas1", 4) != 0)
        return false;
    int depth = 0;
    for (int i = index + 1; i < static_cast<int>(s.size()); i++) {
        if (std::memcmp(s[i].data(), "pas1", 4) == 0)
            depth++;
        else if (std::memcmp(s[i].data(), "pae1", 4) == 0) {
            if (--depth == 0)
                break;
        } else if (depth == 1 && Clyt::IsPane(s[i])) {
            Write<float>(s[i].data(), 0x24, Read<float>(s[i].data(), 0x24) + dx);
            Write<float>(s[i].data(), 0x28, Read<float>(s[i].data(), 0x28) + dy);
        }
    }
    entry->data = c->Build();
    return true;
}

int HidePanes(std::vector<ArcEntry>& arc, const std::string& layout,
              const std::vector<std::string>& panes) {
    ArcEntry* entry = FindEntry(arc, layout);
    if (!entry)
        return 0;
    int hidden = 0;
    for (const auto& name : panes) {
        const u32 pane = FindPane(entry->data.data(), static_cast<u32>(entry->data.size()), name);
        if (pane != 0) {
            SetPaneVisible(entry->data.data(), pane, false);
            hidden++;
        }
    }
    return hidden;
}

} // namespace Leaf::LayoutPatch
