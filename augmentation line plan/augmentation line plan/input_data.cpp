///////////////////////////////////////////////////////////////////////////
/// @file input_data.cpp
/// @brief CSV 读取、对象关联、候选及逐车次任务生成。
///////////////////////////////////////////////////////////////////////////
#include "input_data.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

struct Row {
    std::string context;
    std::map<std::string, std::string> fields;
};

[[noreturn]] void Fail(const std::string& context, const std::string& message) {
    throw InputError{"输入数据", context, message};
}

const std::string& Field(const Row& row, const std::string& name) {
    const auto it = row.fields.find(name);
    if (it == row.fields.end() || it->second.empty())
        Fail(row.context + ":" + name, "缺少必填字段或字段为空。");
    return it->second;
}

Direction ReadSectionDirection(const Row& row) {
    const auto& value = Field(row, "direction");
    if (value == "up") return Direction::Up;
    if (value == "down") return Direction::Down;
    Fail(row.context + ":direction", "区间方向必须为 up 或 down：" + value);
}

int Number(const std::string& value, const std::string& context, int minimum = 0) {
    int result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || result < minimum)
        Fail(context, "无效整数或超出允许范围：" + value);
    return result;
}

int Integer(const Row& row, const std::string& key, int minimum = 0) {
    return Number(Field(row, key), row.context + ":" + key, minimum);
}

double Decimal(const Row& row, const std::string& key) {
    const auto& value = Field(row, key);
    double result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()
        || !std::isfinite(result) || result < 0)
        Fail(row.context + ":" + key, "必须是有限非负数：" + value);
    return result;
}

int CheckedInt(std::int64_t value, const std::string& context) {
    if (value < 0 || value > std::numeric_limits<int>::max())
        Fail(context, "计算结果超出非负整数范围。");
    return static_cast<int>(value);
}

std::vector<std::string> Split(const Row& row, const std::string& key, bool trailing = false) {
    auto value = Field(row, key);
    if (trailing && value.back() == ';') value.pop_back();
    std::vector<std::string> result;
    std::size_t start = 0;
    while (true) {
        const auto end = value.find(';', start);
        auto part = value.substr(start, end - start);
        if (part.empty()) Fail(row.context + ":" + key, "分号列表含空元素。");
        result.push_back(std::move(part));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return result;
}

// 校验编码，避免系统代码页或非法字节被当成车站名称。
void ValidateUtf8(const std::string& text, const std::string& context) {
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first < 0x80) {
            if (first == 0) Fail(context, "CSV 含 NUL 字节。");
            continue;
        }
        int count = 0;
        std::uint32_t code = 0;
        if (first >= 0xc2 && first <= 0xdf) { count = 1; code = first & 0x1f; }
        else if (first >= 0xe0 && first <= 0xef) { count = 2; code = first & 0x0f; }
        else if (first >= 0xf0 && first <= 0xf4) { count = 3; code = first & 0x07; }
        else Fail(context, "文件不是有效 UTF-8。");
        const int length = count;
        while (count-- > 0) {
            if (i == text.size()) Fail(context, "UTF-8 字符被截断。");
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) Fail(context, "无效 UTF-8 连续字节。");
            code = (code << 6) | (next & 0x3f);
        }
        if ((length == 2 && code < 0x800) || (length == 3 && code < 0x10000)
            || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
            Fail(context, "无效 UTF-8 码点。");
    }
}

std::vector<Row> ReadCsv(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    const std::string filename(bytes.begin(), bytes.end());
    std::ifstream input(path, std::ios::binary);
    if (!input) Fail(filename, "无法打开文件。");
    std::string text((std::istreambuf_iterator<char>(input)), {});
    if (input.bad()) Fail(filename, "读取文件失败。");
    ValidateUtf8(text, filename);
    if (text.starts_with("\xef\xbb\xbf")) text.erase(0, 3);
    std::vector<Row> rows;
    std::vector<std::string> header, cells;
    std::string cell;
    bool quoted = false, closed = false;
    std::size_t line = 1, record_line = 1;
    const auto finish = [&]() {
        cells.push_back(std::move(cell));
        cell.clear();
        if (cells.size() == 1 && cells.front().empty()) { cells.clear(); return; }
        const auto context = filename + ":" + std::to_string(record_line);
        if (header.empty()) {
            std::set<std::string> names;
            for (const auto& name : cells)
                if (name.empty() || !names.insert(name).second) Fail(context, "表头为空或重复。");
            header = cells;
        } else {
            if (cells.size() != header.size()) Fail(context, "列数与表头不一致。");
            Row row{context, {}};
            for (std::size_t i = 0; i < header.size(); ++i) row.fields[header[i]] = cells[i];
            rows.push_back(std::move(row));
        }
        cells.clear();
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (quoted) {
            if (ch == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') { cell += '"'; ++i; }
                else { quoted = false; closed = true; }
            } else { cell += ch; if (ch == '\n') ++line; }
        } else if (ch == ',') {
            cells.push_back(std::move(cell)); cell.clear(); closed = false;
        } else if (ch == '\r' || ch == '\n') {
            finish(); closed = false;
            if (ch == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            record_line = ++line;
        } else if (ch == '"' && cell.empty() && !closed) {
            quoted = true;
        } else {
            if (closed || ch == '"') Fail(filename + ":" + std::to_string(line), "CSV 引号格式错误。");
            cell += ch;
        }
    }
    if (quoted) Fail(filename + ":" + std::to_string(record_line), "CSV 引号未闭合。");
    if (!cell.empty() || !cells.empty() || closed) finish();
    if (header.empty()) Fail(filename, "CSV 缺少表头。");
    const std::map<std::string, std::vector<std::string>> required_columns{
        {"parameters.csv", {"parameter", "value", "unit"}},
        {"stations.csv", {"station_id", "station_name"}},
        {"platforms.csv", {"platform_id", "station_id", "platform_name", "total_available_time"}},
        {"sections.csv", {"section_id", "from_station", "to_station", "maximum_train_count", "pure_running_time"}},
        {"timetable.csv", {"trainID", "FromNode", "ToNode", "Paths", "Times"}},
        {"lines.csv", {"line_id", "stations", "stop_plan"}},
        {"line_allowed_platforms.csv", {"line_id", "station_name", "stop_plan", "platform_ids", "platform_name"}},
        {"ods.csv", {"od_id", "origin_station", "destination_station", "demand"}}
    };
    for (const auto& column : required_columns.at(path.filename().string()))
        if (std::find(header.begin(), header.end(), column) == header.end())
            Fail(filename + ":1:" + column, "缺少必需表头。");
    return rows;
}

std::vector<Row> ReadOrdered(const std::filesystem::path& directory,
                            const std::string& file, const std::string& id) {
    auto rows = ReadCsv(directory / file);
    std::set<int> ids;
    for (const auto& row : rows)
        if (!ids.insert(Integer(row, id)).second) Fail(row.context + ":" + id, "业务 ID 重复。");
    std::sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
        return Integer(a, id) < Integer(b, id);
    });
    return rows;
}

template <typename Map, typename Key>
const auto& Find(const Map& map, const Key& key, const std::string& context) {
    const auto it = map.find(key);
    if (it == map.end()) Fail(context, "引用不存在的对象。");
    return it->second;
}

struct Visit {
    int station_id;
    int platform_id;
    bool stop;
    int start;
    int end;
};

struct ExistingRecord {
    Row source;
    std::vector<Visit> visits;
};

/// @brief 已按业务 ID 编号的 OD 输入，供线路构建及后续 OD 服务计算共用。
struct OdRecord {
    int id;                     ///< ods.csv:od_id
    int index;                  ///< [内部编号] 按业务 ID 排序
    const Station* origin;      ///< ods.csv:origin_station
    const Station* destination; ///< ods.csv:destination_station
    double demand;              ///< ods.csv:demand
    std::string context;        ///< [内部计算] CSV 诊断位置
};

// 临时构建器不向公开数据集泄露 CSV 行引用。
class Builder {
public:
    explicit Builder(std::filesystem::path directory) : directory_(std::move(directory)) {}

    ProblemData Build() {
        ReadParameters();
        ReadInfrastructureAndTimetable();
        BuildInfrastructure();
        ReadOdRecords();
        BuildCandidateLines();
        BuildExistingTrains();
        BuildCandidates();
        BuildOds();
        return std::move(data_);
    }

private:
    void ReadParameters() {
        std::map<std::string, Row> rows;
        for (const auto& row : ReadCsv(directory_ / "parameters.csv"))
            if (!rows.emplace(Field(row, "parameter"), row).second)
                Fail(row.context + ":parameter", "参数重复。");
        const auto parameter = [&](const std::string& key, const std::string& unit) -> const Row& {
            const auto& row = Find(rows, key, "parameters.csv:" + key);
            if (Field(row, "unit") != unit) Fail(row.context + ":unit", "参数单位应为 " + unit);
            return row;
        };
        const auto integer = [&](const std::string& key, const std::string& unit, int minimum = 0) {
            return Integer(parameter(key, unit), "value", minimum);
        };
        auto& p = data_.parameters;
        p.planning_horizon = integer("planning_horizon", "time_step", 1);
        if (rows.contains("time_step_seconds")) {
            if (rows.contains("time_step_minutes"))
                Fail("parameters.csv", "time_step_seconds 与 time_step_minutes 只能指定一个。");
            p.time_step_seconds = integer("time_step_seconds", "second_per_time_step", 1);
            p.time_step_minutes = 0;
        } else {
            p.time_step_minutes = integer("time_step_minutes", "minute_per_time_step", 1);
        }
        if (rows.contains("existing_timing_mode")) {
            p.existing_timing_mode = integer("existing_timing_mode", "mode");
            if (p.existing_timing_mode > 1)
                Fail("parameters.csv:existing_timing_mode", "既有时分模式只能为0或1。");
        }
        p.unmet_demand_penalty_factor = Decimal(parameter("unmet_demand_penalty_factor", "dimensionless"), "value");
        p.track_headway = integer("track_headway", "time_step");
        p.arrival_headway = integer("arrival_headway", "time_step");
        p.departure_headway = integer("departure_headway", "time_step");
        p.minimum_stop_occupation = integer("minimum_stop_platform_occupation_time", "time_step");
        p.minimum_pass_occupation = integer("minimum_pass_platform_occupation_time", "time_step");
        p.minimum_terminal_occupation = integer("minimum_terminal_platform_occupation_time", "time_step");
        p.existing_train_max_delay = integer("existing_train_max_delay", "time_step");
        p.startup_additional_time = integer("startup_additional_time", "time_step");
        p.stopping_additional_time = integer("stopping_additional_time", "time_step");
        // 原有求解控制参数为必填；新增核配置在下方显式保留历史输入的兼容默认值。
        // second 是墙钟秒，不随运行图时间步换算。
        p.maximum_rounds = integer("maximum_rounds", "round", 1);
        p.total_time_limit_seconds = Decimal(parameter("total_time_limit_seconds", "second"), "value");
        p.master_time_limit_seconds = Decimal(parameter("master_time_limit_seconds", "second"), "value");
        const auto& master_logging_row = parameter("master_solver_logging", "boolean");
        const int master_logging = Integer(master_logging_row, "value", 0); // 主问题日志仅接受 0/1。
        if (master_logging > 1) Fail(master_logging_row.context + ":value", "日志开关必须为 0 或 1。");
        p.master_solver_logging = master_logging == 1;
        p.timetable_time_limit_seconds = Decimal(parameter("timetable_time_limit_seconds", "second"), "value");
        const auto& logging_row = parameter("timetable_solver_logging", "boolean");
        const int logging = Integer(logging_row, "value", 0); // 只允许 0=关闭、1=开启。
        if (logging > 1) Fail(logging_row.context + ":value", "日志开关必须为 0 或 1。");
        p.timetable_solver_logging = logging == 1;
        // 每次运行都显式读取后端选择；缺失或非法值不得静默切回 MILP。
        p.timetable_solver = integer("timetable_solver", "backend", 1);
        if (p.timetable_solver != 1 && p.timetable_solver != 2)
            Fail("parameters.csv:timetable_solver", "时刻表求解器只能为 1（MILP）或 2（SAT）。");
        // 新参数可省略以直接复验历史输入；显式配置时严格检查，不静默改换模式。
        if (rows.contains("conflict_cut_mode"))
            p.conflict_cut_mode = integer("conflict_cut_mode", "mode", 1);
        if (p.conflict_cut_mode > 2 || (p.conflict_cut_mode == 2 && p.timetable_solver != 2))
            Fail("parameters.csv:conflict_cut_mode", "割模式只能为1或2，assumptions模式必须使用SAT后端。");
        if (rows.contains("core_verification_time_limit_seconds"))
            p.core_verification_time_limit_seconds = Decimal(
                parameter("core_verification_time_limit_seconds", "second"), "value");
        p.planning_output_root = Field(parameter("planning_output_root", "path"), "value");
        if (p.planning_output_root.empty()) Fail("parameters.csv:planning_output_root", "结果目录不能为空。");
        const auto& frequency = parameter("candidate_frequencies", "train_count");
        std::set<int> unique;
        for (const auto& value : Split(frequency, "value")) {
            const int f = Number(value, frequency.context + ":value", 1);
            if (!unique.insert(f).second) Fail(frequency.context + ":value", "频率重复。");
        }
        p.candidate_frequencies.assign(unique.begin(), unique.end());
        for (const std::string name : {"long", "short"}) {
            data_.rolling_stocks.push_back(std::make_unique<RollingStock>(
                static_cast<int>(data_.rolling_stocks.size()),
                integer(name + "_seat_capacity", "seat", 1),
                integer(name + "_available_units", "trainset"),
                integer(name + "_minimum_turnaround_time", "time_step")));
            data_.rolling_stock_names.push_back(name);
        }
    }

    void ReadInfrastructureAndTimetable() {
        station_rows_ = ReadOrdered(directory_, "stations.csv", "station_id");
        platform_rows_ = ReadOrdered(directory_, "platforms.csv", "platform_id");
        section_rows_ = ReadOrdered(directory_, "sections.csv", "section_id");
        if (station_rows_.empty()) Fail("stations.csv", "车站集合不能为空。");
        for (const auto& row : station_rows_) {
            const int id = Integer(row, "station_id");
            if (!station_ids_.emplace(Field(row, "station_name"), id).second)
                Fail(row.context + ":station_name", "车站名称重复。");
            station_names_[id] = Field(row, "station_name");
        }
        for (const auto& row : platform_rows_) {
            const int station = Integer(row, "station_id");
            Find(station_names_, station, row.context + ":station_id");
            const auto key = std::make_pair(station, Field(row, "platform_name"));
            if (!platform_ids_.emplace(key, Integer(row, "platform_id")).second)
                Fail(row.context + ":platform_name", "站内轨道名称重复。");
        }
        for (const auto& row : section_rows_) {
            const int from = Find(station_ids_, Field(row, "from_station"), row.context + ":from_station");
            const int to = Find(station_ids_, Field(row, "to_station"), row.context + ":to_station");
            if (from == to) Fail(row.context, "区间两端不能相同。");
            const int id = Integer(row, "section_id");
            if (!section_ids_.emplace(std::make_pair(from, to), id).second)
                Fail(row.context, "有向区间重复。");
            pure_times_[id] = Integer(row, "pure_running_time", 1);
        }
        auto rows = ReadCsv(directory_ / "timetable.csv");
        std::set<std::string> ids;
        for (const auto& row : rows)
            if (!ids.insert(Field(row, "trainID")).second) Fail(row.context + ":trainID", "列车 ID 重复。");
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            return Field(a, "trainID") < Field(b, "trainID");
        });
        for (const auto& row : rows) ReadExistingRecord(row);
    }

    void ReadExistingRecord(const Row& row) {
        const auto paths = Split(row, "Paths", true);
        const auto times = Split(row, "Times", true);
        if (paths.size() < 4 || paths.size() % 2 != 0)
            Fail(row.context + ":Paths", "路径必须为至少两站的轨道配对序列。");
        const bool legacy_terminal_slots = times.size() == paths.size();
        if (!legacy_terminal_slots && times.size() != paths.size() - 2)
            Fail(row.context + ":Paths/Times", "时刻必须包含实际始发、中间站到发和实际终到。");
        // 旧格式有首末占轨槽位。仅用它们识别实际时刻在序列中的位置。
        const std::size_t time_offset = legacy_terminal_slots ? 0 : 1;
        ExistingRecord record{row, {}};
        std::set<int> visited;
        for (std::size_t i = 0; i < paths.size(); i += 2) {
            if (paths[i] != paths[i + 1]) Fail(row.context + ":Paths", "每站必须连续出现两次相同轨道。");
            const auto split = paths[i].rfind('-');
            if (split == std::string::npos) Fail(row.context + ":Paths", "路径应为站名-轨道名。");
            const int station = Find(station_ids_, paths[i].substr(0, split), row.context + ":Paths");
            if (!visited.insert(station).second) Fail(row.context + ":Paths", "列车路径含重复车站。");
            const auto name = paths[i].substr(split + 1);
            const int platform = Find(platform_ids_, std::make_pair(station, name), row.context + ":Paths");
            if (name.empty() || (name[0] != 't' && name[0] != 'b'))
                Fail(row.context + ":Paths", "当前输入约定使用 t 停站轨道或 b 通过轨道。");
            const bool stop = name[0] == 't';
            const bool origin = i == 0;
            const bool destination = i + 2 == paths.size();
            const bool terminal = origin || destination;
            if (terminal && !stop)
                Fail(row.context + ":Paths", "始发与终到必须停车。");
            const auto context = row.context + ":Times";
            const auto& parameters = data_.parameters;
            std::int64_t start = origin ? 0 : Number(times[i - time_offset], context);
            std::int64_t end = destination ? 0 : Number(times[i + 1 - time_offset], context);
            if (origin) start = end - parameters.minimum_terminal_occupation;
            if (destination) end = start + parameters.minimum_terminal_occupation;
            if (start < 0 || end < start || end > parameters.planning_horizon)
                Fail(context, "实际到发或派生的基准占轨时刻逆序或超出规划时域。");
            if (!terminal && ((!stop && start != end)
                || (stop && parameters.existing_timing_mode == 0 && end - start < parameters.minimum_stop_occupation)))
                Fail(context, "中间停站/通过占轨时间不符合参数约定。");
            const auto occupation = stop ? end - start : parameters.minimum_pass_occupation;
            occupied_[platform] = CheckedInt(static_cast<std::int64_t>(occupied_[platform]) + occupation, row.context);
            if (!record.visits.empty()) {
                const auto& previous = record.visits.back();
                const int section = Find(section_ids_, std::make_pair(previous.station_id, station), row.context + ":Paths");
                const auto expected = static_cast<std::int64_t>(pure_times_.at(section))
                    + (previous.stop ? data_.parameters.startup_additional_time : 0)
                    + (stop ? data_.parameters.stopping_additional_time : 0);
                if (start <= previous.end)
                    Fail(row.context + ":Times", "既有区间运行时长必须为正。");
                if (parameters.existing_timing_mode == 0 && static_cast<std::int64_t>(start) - previous.end != expected)
                    Fail(row.context + ":Times", "区间时间不等于纯运行时间加起停附加时分。");
                counts_[section] = CheckedInt(static_cast<std::int64_t>(counts_[section]) + 1, row.context);
            }
            record.visits.push_back({station, platform, stop, static_cast<int>(start), static_cast<int>(end)});
        }
        if (Field(row, "FromNode") != station_names_.at(record.visits.front().station_id)
            || Field(row, "ToNode") != station_names_.at(record.visits.back().station_id))
            Fail(row.context + ":FromNode/ToNode", "始终点与路径不一致。");
        existing_records_.push_back(std::move(record));
    }

    void BuildInfrastructure() {
        std::map<int, int> station_indices;
        for (std::size_t i = 0; i < station_rows_.size(); ++i)
            station_indices[Integer(station_rows_[i], "station_id")] = CheckedInt(i, "stations.csv");
        for (const auto& row : platform_rows_) {
            const int id = Integer(row, "platform_id");
            const int total = Integer(row, "total_available_time");
            if (occupied_[id] > total) Fail(row.context + ":total_available_time", "既有占用超过总可用时间。");
            auto item = std::make_unique<Platform>(CheckedInt(data_.platforms.size(), row.context),
                Field(row, "platform_name"), station_indices.at(Integer(row, "station_id")), occupied_[id], total);
            data_.platform_by_id[id] = item.get();
            data_.platforms.push_back(std::move(item));
        }
        for (const auto& row : station_rows_) {
            const int id = Integer(row, "station_id");
            std::vector<const Platform*> platforms;
            for (const auto& platform : data_.platforms)
                if (platform->station_index() == station_indices.at(id)) platforms.push_back(platform.get());
            auto item = std::make_unique<Station>(station_indices.at(id), Field(row, "station_name"), std::move(platforms));
            data_.station_by_id[id] = item.get();
            data_.station_by_name[item->name()] = item.get();
            data_.stations.push_back(std::move(item));
        }
        for (const auto& row : section_rows_) {
            const int id = Integer(row, "section_id");
            const int maximum = Integer(row, "maximum_train_count");
            if (counts_[id] > maximum) Fail(row.context + ":maximum_train_count", "既有车次数超过区间容量。");
            auto item = std::make_unique<Section>(CheckedInt(data_.sections.size(), row.context),
                data_.station_by_name.at(Field(row, "from_station")),
                data_.station_by_name.at(Field(row, "to_station")), ReadSectionDirection(row),
                counts_[id], maximum);
            data_.section_by_id[id] = item.get();
            data_.pure_running_times.push_back(pure_times_.at(id));
            data_.sections.push_back(std::move(item));
        }
    }

    std::vector<const Section*> RouteSections(const std::vector<const Station*>& stations,
                                              const std::string& context) const {
        std::vector<const Section*> sections;
        for (std::size_t i = 1; i < stations.size(); ++i) {
            const int from = station_ids_.at(stations[i - 1]->name());
            const int to = station_ids_.at(stations[i]->name());
            sections.push_back(data_.section_by_id.at(Find(section_ids_, std::make_pair(from, to), context)));
        }
        return sections;
    }

    std::vector<int> RunningTimes(const Line& line) const {
        std::vector<int> times;
        const auto stops = [&](std::size_t position) {
            return std::binary_search(line.stop_positions().begin(), line.stop_positions().end(), position);
        };
        for (std::size_t i = 0; i < line.sections().size(); ++i) {
            const auto value = static_cast<std::int64_t>(data_.pure_running_times.at(line.sections()[i]->index()))
                + (stops(i) ? data_.parameters.startup_additional_time : 0)
                + (stops(i + 1) ? data_.parameters.stopping_additional_time : 0);
            times.push_back(CheckedInt(value, "线路 " + std::to_string(line.index()) + ":运行时间"));
        }
        return times;
    }

    std::vector<int> DwellTimes(const Line& line) const {
        std::vector<int> times(line.stations().size(), 0);
        for (const auto position : line.stop_positions())
            if (position > 0 && position + 1 < times.size()) times[position] = data_.parameters.minimum_stop_occupation;
        return times;
    }

    void ReadOdRecords() {
        for (const auto& row : ReadOrdered(directory_, "ods.csv", "od_id")) {
            const auto* origin = Find(data_.station_by_name, Field(row, "origin_station"), row.context + ":origin_station");
            const auto* destination = Find(data_.station_by_name, Field(row, "destination_station"), row.context + ":destination_station");
            if (origin == destination) Fail(row.context, "OD 起终点不能相同。");
            od_records_.push_back({Integer(row, "od_id"), CheckedInt(od_records_.size(), row.context),
                                   origin, destination, Decimal(row, "demand"), row.context});
        }
    }

    std::vector<std::vector<int>> BuildStopSectionOdIndices(
        const std::vector<const Station*>& stations,
        const std::vector<std::size_t>& stops) const {
        std::vector<std::vector<int>> covered_ods(stops.size() - 1);
        for (const auto& od : od_records_) {
            const auto first = std::find_if(stops.begin(), stops.end(), [&](std::size_t position) {
                return stations[position] == od.origin;
            });
            if (first == stops.end()) continue;
            const auto last = std::find_if(first + 1, stops.end(), [&](std::size_t position) {
                return stations[position] == od.destination;
            });
            if (last == stops.end()) continue;
            for (auto stop = first; stop != last; ++stop) {
                covered_ods[stop - stops.begin()].push_back(od.index);
            }
        }
        return covered_ods;
    }

    void BuildCandidateLines() {
        const auto rows = ReadOrdered(directory_, "lines.csv", "line_id");
        std::map<std::pair<int, std::string>, Row> allocations;
        for (const auto& row : ReadCsv(directory_ / "line_allowed_platforms.csv")) {
            const auto key = std::make_pair(Integer(row, "line_id"), Field(row, "station_name"));
            if (!allocations.emplace(key, row).second) Fail(row.context, "线路-车站轨道记录重复。");
        }
        std::set<std::pair<int, std::string>> used;
        for (const auto& row : rows) {
            const int id = Integer(row, "line_id");
            const auto names = Split(row, "stations");
            const auto stop_flags = Split(row, "stop_plan");
            if (names.size() < 2 || names.size() != stop_flags.size()) Fail(row.context, "车站和停站序列长度不一致。");
            std::vector<const Station*> stations;
            std::vector<std::size_t> stops;
            std::vector<std::vector<const Platform*>> allowed;
            std::set<std::string> unique_stations;
            for (std::size_t i = 0; i < names.size(); ++i) {
                if (!unique_stations.insert(names[i]).second) Fail(row.context + ":stations", "线路含重复车站。");
                const auto* station = Find(data_.station_by_name, names[i], row.context + ":stations");
                stations.push_back(station);
                const int stop = Number(stop_flags[i], row.context + ":stop_plan");
                if (stop > 1 || ((i == 0 || i + 1 == names.size()) && stop != 1))
                    Fail(row.context + ":stop_plan", "停站标志必须为 0/1，始终点必须停车。");
                if (stop) stops.push_back(i);
                const auto key = std::make_pair(id, names[i]);
                const auto& allocation = Find(allocations, key, row.context + ":line_allowed_platforms");
                used.insert(key);
                if (Integer(allocation, "stop_plan") != stop) Fail(allocation.context + ":stop_plan", "与线路停站标志不一致。");
                const auto ids = Split(allocation, "platform_ids");
                const auto tracks = Split(allocation, "platform_name");
                if (ids.size() != tracks.size()) Fail(allocation.context, "轨道 ID 与名称数量不一致。");
                std::vector<const Platform*> choices;
                std::set<int> unique;
                for (std::size_t j = 0; j < ids.size(); ++j) {
                    const int pid = Number(ids[j], allocation.context + ":platform_ids");
                    const auto* platform = Find(data_.platform_by_id, pid, allocation.context + ":platform_ids");
                    if (!unique.insert(pid).second || platform->station_index() != station->index()
                        || platform->name() != tracks[j] || tracks[j][0] != (stop ? 't' : 'b'))
                        Fail(allocation.context, "轨道重复、归属/名称或停通类型不一致。");
                    choices.push_back(platform);
                }
                allowed.push_back(std::move(choices));
            }
            auto sections = RouteSections(stations, row.context + ":stations");
            auto covered_ods = BuildStopSectionOdIndices(stations, stops);
            std::vector<const RollingStock*> stock;
            for (const auto& item : data_.rolling_stocks) stock.push_back(item.get());
            auto line = std::make_unique<Line>(CheckedInt(data_.lines.size(), row.context),
                std::move(stations), std::move(sections), std::move(stops), std::move(allowed),
                data_.parameters.candidate_frequencies, std::move(stock), std::move(covered_ods));
            data_.line_by_id[id] = line.get();
            data_.lines.push_back(std::move(line));
        }
        for (const auto& [key, row] : allocations)
            if (!used.contains(key)) Fail(row.context, "轨道分配引用未使用的线路或车站。");
    }

    void BuildExistingTrains() {
        for (const auto& record : existing_records_) {
            std::vector<const Station*> stations;
            std::vector<std::size_t> stops;
            std::vector<std::vector<const Platform*>> allowed;
            std::vector<std::optional<int>> arrivals, departures, platforms;
            for (std::size_t i = 0; i < record.visits.size(); ++i) {
                const auto& visit = record.visits[i];
                stations.push_back(data_.station_by_id.at(visit.station_id));
                if (visit.stop) stops.push_back(i);
                const auto* platform = data_.platform_by_id.at(visit.platform_id);
                allowed.push_back({platform});
                platforms.push_back(platform->index());
                arrivals.push_back(i == 0 ? std::nullopt : std::optional<int>(visit.start));
                departures.push_back(i + 1 == record.visits.size() ? std::nullopt : std::optional<int>(visit.end));
            }
            auto sections = RouteSections(stations, record.source.context + ":Paths");
            // 既有专用线路编号接在候选线路之后，不进入可选服务集合。
            auto line = std::make_unique<Line>(
                CheckedInt(data_.lines.size() + data_.existing_lines.size(), record.source.context),
                std::move(stations), std::move(sections), std::move(stops), std::move(allowed),
                std::vector<int>{}, std::vector<const RollingStock*>{});
            auto running = RunningTimes(*line);
            auto dwell = DwellTimes(*line);
            if (data_.parameters.existing_timing_mode == 1) {
                for (std::size_t i = 1; i < record.visits.size(); ++i)
                    running[i - 1] = record.visits[i].start - record.visits[i - 1].end;
                for (std::size_t i = 1; i + 1 < record.visits.size(); ++i)
                    dwell[i] = record.visits[i].end - record.visits[i].start;
            }
            auto train = std::make_unique<Train>(CheckedInt(data_.existing_trains.size(), record.source.context),
                TrainKind::Existing, line.get(), std::move(running), std::move(dwell),
                std::move(arrivals), std::move(departures), std::move(platforms), std::nullopt,
                record.visits.front().start, record.visits.back().end);
            data_.existing_train_by_id[Field(record.source, "trainID")] = train.get();
            data_.existing_lines.push_back(std::move(line));
            data_.existing_trains.push_back(std::move(train));
        }
    }

    void BuildCandidates() {
        for (const auto& line : data_.lines) {
            const auto running = RunningTimes(*line);
            const auto dwell = DwellTimes(*line);
            std::int64_t total = 0;
            for (int value : running) total += value;
            for (int value : dwell) total += value;
            const int trip = CheckedInt(total, "候选任务最短时间");
            std::vector<int> occupations(line->stations().size(), data_.parameters.minimum_pass_occupation);
            for (const auto position : line->stop_positions()) occupations[position] = data_.parameters.minimum_stop_occupation;
            occupations.front() = data_.parameters.minimum_terminal_occupation;
            occupations.back() = data_.parameters.minimum_terminal_occupation;
            for (int frequency : line->allowed_frequencies()) {
                for (const auto* stock : line->allowed_rolling_stocks()) {
                    const int index = CheckedInt(data_.candidates.size(), "候选编号");
                    std::vector<const Train*> trains;
                    for (int ordinal = 0; ordinal < frequency; ++ordinal) {
                        const auto size = line->stations().size();
                        const int train_index = CheckedInt(data_.existing_trains.size() + data_.added_trains.size(), "列车编号");
                        auto train = std::make_unique<Train>(train_index, TrainKind::Added, line.get(), running, dwell,
                            std::vector<std::optional<int>>(size), std::vector<std::optional<int>>(size),
                            std::vector<std::optional<int>>(size), index,
                            std::nullopt, std::nullopt);
                        trains.push_back(train.get());
                        data_.added_trains.push_back(std::move(train));
                    }
                    data_.candidates.push_back(std::make_unique<CandidatePlan>(index, line.get(), frequency, stock,
                        trip, occupations, std::move(trains)));
                }
            }
        }
    }

    void BuildOds() {
        for (const auto& record : od_records_) {
            const auto* origin = record.origin;
            const auto* destination = record.destination;
            std::vector<const CandidatePlan*> plans;
            std::vector<OdService> services;
            int shortest = std::numeric_limits<int>::max();
            for (const auto& plan : data_.candidates) {
                const auto* line = plan->line();
                if (!line->HasDirectStopService(origin->index(), destination->index())) continue;
                const auto& stations = line->stations();
                const auto start = std::find(stations.begin(), stations.end(), origin) - stations.begin();
                const auto end = std::find(stations.begin(), stations.end(), destination) - stations.begin();
                const auto* train = plan->trains().front();
                std::int64_t time = 0;
                for (auto i = start; i < end; ++i) {
                    time += train->section_running_times()[i];
                    if (i > start) time += train->minimum_dwell_times()[i];
                }
                const int travel = CheckedInt(time, record.context + ":旅行时间");
                plans.push_back(plan.get());
                services.push_back({plan.get(), travel});
                shortest = std::min(shortest, travel);
            }
            // 当前惩罚定义依赖候选最短旅行时间，无服务时不伪造参数。
            if (plans.empty()) Fail(record.context, "OD 没有顺向直达候选，无法计算最短旅行时间和惩罚。");
            const double penalty = shortest * data_.parameters.unmet_demand_penalty_factor;
            if (!std::isfinite(penalty)) Fail(record.context, "未满足需求惩罚溢出。");
            auto od = std::make_unique<OD>(record.index, origin->index(), origin->name(),
                destination->index(), destination->name(), record.demand, shortest, penalty, std::move(plans));
            data_.od_by_id[record.id] = od.get();
            data_.ods.push_back(std::move(od));
            data_.od_services.push_back(std::move(services));
        }
    }

    std::filesystem::path directory_;
    ProblemData data_;
    std::vector<Row> station_rows_, platform_rows_, section_rows_;
    std::vector<ExistingRecord> existing_records_;
    std::vector<OdRecord> od_records_;
    std::map<std::string, int> station_ids_;
    std::map<int, std::string> station_names_;
    std::map<int, int> pure_times_, occupied_, counts_;
    std::map<std::pair<int, std::string>, int> platform_ids_;
    std::map<std::pair<int, int>, int> section_ids_;
};

}  // namespace

InputResult<ProblemData> LoadProblemData(const std::filesystem::path& directory) {
    try {
        return Builder(directory).Build();
    } catch (const InputError& error) {
        return error;
    } catch (const std::exception& error) {
        return InputError{"对象构建", "LoadProblemData", error.what()};
    }
}

void PrintInputReport(const ProblemData& data, std::ostream& output) {
    output << "数据加载完成：车站=" << data.stations.size() << "，轨道=" << data.platforms.size()
           << "，有向区间=" << data.sections.size() << "，既有列车=" << data.existing_trains.size()
           << "，候选线路=" << data.lines.size() << "，候选方案=" << data.candidates.size()
           << "，新增备选任务=" << data.added_trains.size() << "，OD=" << data.ods.size() << '\n';
    output << "规划时域=" << data.parameters.planning_horizon << " time_step，每步="
           << data.parameters.SecondsPerStep() << " 秒；既有实际到发事件允许后移上限="
           << data.parameters.existing_train_max_delay << " time_step，到发线固定。\n";
    for (const auto& [id, section] : data.section_by_id)
        output << "区间 " << id << ' ' << section->from_station()->name() << "->" << section->to_station()->name()
               << " 既有=" << section->existing_train_count() << " 剩余=" << section->remaining_train_capacity() << '\n';
    for (const auto& [id, platform] : data.platform_by_id)
        output << "轨道 " << id << " 既有占用=" << platform->existing_occupied_time()
               << " 剩余=" << platform->remaining_available_time() << '\n';
    for (const auto& [line_id, line] : data.line_by_id) {
        for (const auto& plan : data.candidates) {
            if (plan->line() != line) continue;
            output << "候选 " << plan->index() << " line_id=" << line_id << " frequency=" << plan->frequency()
                   << " formation=" << data.rolling_stock_names.at(plan->rolling_stock()->index())
                   << " T=" << plan->minimum_trip_time() << " tasks=";
            for (std::size_t i = 0; i < plan->trains().size(); ++i)
                output << (i == 0 ? "" : ";") << i + 1 << ':' << plan->trains()[i]->index();
            output << '\n';
        }
    }
    for (const auto& [id, od] : data.od_by_id)
        output << "OD " << id << ' ' << od->origin_name() << "->" << od->destination_name()
               << " 候选数=" << od->candidate_plans().size() << " 最短旅行时间=" << od->shortest_travel_time()
               << " 未满足单位惩罚=" << od->unmet_unit_penalty() << '\n';
    output << "当前仅输出输入数据报告，尚未执行求解。\n"
           << "正式主程序按 main.cpp 指定的输入目录执行完整迭代；第一层后端由 parameters.csv 的 timetable_solver 选择。\n";
}
