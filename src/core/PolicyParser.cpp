#include "diag/core/PlatformPolicy.h"

#include <cctype>
#include <fstream>
#include <iterator>
#include <utility>

namespace {

struct PolicyDeviceInventory {
    std::string name;
    std::string bdf;
    std::string slot;
    std::string position;
    uint32_t index = 0;
};

struct YamlLine {
    size_t indent = 0;
    std::string text;
};

struct TPUProfile {
    std::string product;
    TPUType tpu_type = TPUType::Unknown;
    uint16_t vendor_id = 0;
    uint16_t device_id = 0;
};

std::string trim(const std::string& value)
{
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }

    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }

    return value.substr(begin, end - begin);
}

size_t leading_spaces(const std::string& value)
{
    size_t count = 0;
    while (count < value.size() && value[count] == ' ') {
        ++count;
    }
    return count;
}

bool starts_with(const std::string& value, const std::string& prefix)
{
    return value.rfind(prefix, 0) == 0;
}

std::string strip_quotes(std::string value)
{
    value = trim(value);
    if (value.size() >= 2 &&
        ((value.front() == '"' && value.back() == '"') ||
         (value.front() == '\'' && value.back() == '\''))) {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

std::string yaml_value(const std::string& line)
{
    auto pos = line.find(':');
    if (pos == std::string::npos) {
        return {};
    }
    return strip_quotes(line.substr(pos + 1));
}

uint64_t parse_u64(const std::string& value)
{
    return static_cast<uint64_t>(std::stoull(strip_quotes(value), nullptr, 0));
}

uint16_t parse_u16(const std::string& value)
{
    return static_cast<uint16_t>(parse_u64(value));
}

TPUType tpu_type_from_product(const std::string& product)
{
    if (product == "atlas") {
        return TPUType::Atlas;
    }
    if (product == "atlas_m") {
        return TPUType::AtlasM;
    }
    return TPUType::Unknown;
}

std::vector<YamlLine> read_yaml_lines(const std::string& path)
{
    std::ifstream input(path);
    std::vector<YamlLine> lines;
    std::string line;

    while (std::getline(input, line)) {
        auto text = trim(line);
        if (text.empty() || starts_with(text, "#")) {
            continue;
        }
        lines.push_back({leading_spaces(line), std::move(text)});
    }

    return lines;
}

size_t find_line(const std::vector<YamlLine>& lines,
                 const std::string& text,
                 size_t begin = 0,
                 size_t end = static_cast<size_t>(-1))
{
    if (end == static_cast<size_t>(-1) || end > lines.size()) {
        end = lines.size();
    }
    for (size_t i = begin; i < end; ++i) {
        if (lines[i].text == text) {
            return i;
        }
    }
    return lines.size();
}

size_t section_end(const std::vector<YamlLine>& lines, size_t section)
{
    if (section >= lines.size()) {
        return lines.size();
    }

    auto indent = lines[section].indent;
    for (size_t i = section + 1; i < lines.size(); ++i) {
        if (lines[i].indent <= indent) {
            return i;
        }
    }
    return lines.size();
}

TPUProfile parse_tpu_profile(const std::vector<YamlLine>& lines)
{
    TPUProfile profile;
    auto section = find_line(lines, "chip_profile:");
    auto profile_end = section_end(lines, section);
    auto match = find_line(lines, "match:", section, profile_end);
    auto match_end = section_end(lines, match);

    for (size_t i = section + 1; i < profile_end; ++i) {
        if (starts_with(lines[i].text, "product:")) {
            profile.product = yaml_value(lines[i].text);
            profile.tpu_type = tpu_type_from_product(profile.product);
            break;
        }
    }

    for (size_t i = match + 1; i < match_end; ++i) {
        if (starts_with(lines[i].text, "vendor_id:")) {
            profile.vendor_id = parse_u16(yaml_value(lines[i].text));
        } else if (starts_with(lines[i].text, "device_id:")) {
            profile.device_id = parse_u16(yaml_value(lines[i].text));
        }
    }

    return profile;
}

std::vector<ModuleInstanceConfig> parse_module_list(const std::vector<YamlLine>& lines,
                                                    const std::string& section_name)
{
    std::vector<ModuleInstanceConfig> modules;
    auto section = find_line(lines, section_name);
    auto end = section_end(lines, section);
    ModuleInstanceConfig* current = nullptr;

    for (size_t i = section + 1; i < end; ++i) {
        if (lines[i].indent == lines[section].indent + 2 &&
            starts_with(lines[i].text, "- index:")) {
            ModuleInstanceConfig module;
            module.index = static_cast<uint32_t>(parse_u64(yaml_value(lines[i].text)));
            modules.push_back(module);
            current = &modules.back();
        } else if (current != nullptr && starts_with(lines[i].text, "reg_offset:")) {
            current->reg_offset = parse_u64(yaml_value(lines[i].text));
        } else if (current != nullptr && starts_with(lines[i].text, "reg_size:")) {
            current->reg_size = parse_u64(yaml_value(lines[i].text));
        }
    }

    return modules;
}

std::vector<ModuleInstanceConfig> parse_dmc_list(const std::vector<YamlLine>& lines,
                                                 size_t dmc_section)
{
    std::vector<ModuleInstanceConfig> dmc_modules;
    auto end = section_end(lines, dmc_section);
    ModuleInstanceConfig* current = nullptr;

    for (size_t i = dmc_section + 1; i < end; ++i) {
        if (lines[i].indent == lines[dmc_section].indent + 2 &&
            starts_with(lines[i].text, "- index:")) {
            ModuleInstanceConfig module;
            module.index = static_cast<uint32_t>(parse_u64(yaml_value(lines[i].text)));
            dmc_modules.push_back(module);
            current = &dmc_modules.back();
        } else if (current != nullptr && starts_with(lines[i].text, "reg_offset:")) {
            current->reg_offset = parse_u64(yaml_value(lines[i].text));
        } else if (current != nullptr && starts_with(lines[i].text, "reg_size:")) {
            current->reg_size = parse_u64(yaml_value(lines[i].text));
        }
    }

    return dmc_modules;
}

std::vector<DDPModuleConfig> parse_ddp_list(const std::vector<YamlLine>& lines)
{
    std::vector<DDPModuleConfig> ddp_modules;
    auto section = find_line(lines, "ddp:");
    auto end = section_end(lines, section);

    for (size_t i = section + 1; i < end; ++i) {
        if (lines[i].indent != lines[section].indent + 2 ||
            !starts_with(lines[i].text, "- index:")) {
            continue;
        }

        DDPModuleConfig ddp;
        ddp.index = static_cast<uint32_t>(parse_u64(yaml_value(lines[i].text)));

        auto next_ddp = end;
        for (size_t j = i + 1; j < end; ++j) {
            if (lines[j].indent == lines[i].indent &&
                starts_with(lines[j].text, "- index:")) {
                next_ddp = j;
                break;
            }
        }

        for (size_t j = i + 1; j < next_ddp; ++j) {
            if (lines[j].indent == lines[i].indent + 2 &&
                starts_with(lines[j].text, "reg_offset:")) {
                ddp.reg_offset = parse_u64(yaml_value(lines[j].text));
            } else if (lines[j].indent == lines[i].indent + 2 &&
                       starts_with(lines[j].text, "reg_size:")) {
                ddp.reg_size = parse_u64(yaml_value(lines[j].text));
            } else if (lines[j].indent == lines[i].indent + 2 &&
                       lines[j].text == "dmc:") {
                ddp.dmc_modules = parse_dmc_list(lines, j);
            }
        }

        ddp_modules.push_back(std::move(ddp));
    }

    return ddp_modules;
}

std::vector<PolicyDeviceInventory> parse_inventory(const std::vector<YamlLine>& lines)
{
    std::vector<PolicyDeviceInventory> inventory;
    auto devices = find_line(lines, "devices:");
    auto end = section_end(lines, devices);

    for (size_t i = devices + 1; i < end; ++i) {
        if (lines[i].indent != lines[devices].indent + 2 ||
            !starts_with(lines[i].text, "- name:")) {
            continue;
        }

        PolicyDeviceInventory device;
        device.name = yaml_value(lines[i].text);

        auto next_device = end;
        for (size_t j = i + 1; j < end; ++j) {
            if (lines[j].indent == lines[i].indent &&
                starts_with(lines[j].text, "- name:")) {
                next_device = j;
                break;
            }
        }

        for (size_t j = i + 1; j < next_device; ++j) {
            if (starts_with(lines[j].text, "bdf:")) {
                device.bdf = yaml_value(lines[j].text);
            } else if (starts_with(lines[j].text, "slot:")) {
                device.slot = yaml_value(lines[j].text);
            } else if (starts_with(lines[j].text, "position:")) {
                device.position = yaml_value(lines[j].text);
            } else if (starts_with(lines[j].text, "index:")) {
                device.index = static_cast<uint32_t>(parse_u64(yaml_value(lines[j].text)));
            }
        }

        inventory.push_back(std::move(device));
    }

    return inventory;
}

std::vector<PolicyEntry> load_one_policy(const std::string& path)
{
    auto lines = read_yaml_lines(path);
    if (lines.empty()) {
        return {};
    }

    auto tpu_profile = parse_tpu_profile(lines);

    TPUDeviceConfig tpu_config;
    tpu_config.pcie_modules = parse_module_list(lines, "pcie:");
    tpu_config.pmu_modules = parse_module_list(lines, "pmu:");
    tpu_config.ddp_modules = parse_ddp_list(lines);
    tpu_config.isi_modules = parse_module_list(lines, "isi:");

    std::vector<PolicyEntry> entries;
    for (const auto& device : parse_inventory(lines)) {
        auto config = tpu_config;
        config.name = device.name;
        config.slot = device.slot;
        config.position = device.position;
        config.tpu_index = device.index;
        entries.push_back({
            device.bdf,
            tpu_profile.vendor_id,
            tpu_profile.device_id,
            tpu_profile.product,
            tpu_profile.tpu_type,
            std::move(config)
        });
    }

    return entries;
}

}

std::vector<PolicyEntry> load_product_policy(const std::vector<std::string>& paths)
{
    std::vector<PolicyEntry> policy;

    for (const auto& path : paths) {
        auto entries = load_one_policy(path);
        policy.insert(policy.end(),
                      std::make_move_iterator(entries.begin()),
                      std::make_move_iterator(entries.end()));
    }

    return policy;
}
