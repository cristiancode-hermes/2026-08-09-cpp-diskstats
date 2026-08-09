// SPDX-License-Identifier: MIT
// cpp-diskstats — Disk I/O monitor for Linux (/proc/diskstats + /sys/block)
// Build: g++ -std=c++23 -O2 -Wall -Wextra src/main.cpp -o diskstats
// Usage: ./diskstats [--watch N] [--json] [--sort KEY] [--top N] [--no-color] [--selftest]

#include <print>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <algorithm>
#include <filesystem>
#include <thread>
#include <chrono>
#include <optional>

namespace fs = std::filesystem;
using std::chrono::steady_clock;

// ── ANSI helpers ──────────────────────────────────────────────────────────
static bool g_color = true;

static std::string c(const char* code) {
  return g_color ? std::string("\x1b[") + code + "m" : std::string{};
}
static const std::string RST   = c("0");
static const std::string BOLD  = c("1");
static const std::string CYAN  = c("36");
static const std::string GREEN = c("32");
static const std::string YEL   = c("33");
static const std::string RED   = c("31");
static const std::string DIM   = c("2");

// ── Data model ────────────────────────────────────────────────────────────
struct DiskStats {
  std::string device;
  uint64_t rd_completed = 0, rd_merged = 0, rd_sectors = 0, rd_ms = 0;
  uint64_t wr_completed = 0, wr_merged = 0, wr_sectors = 0, wr_ms = 0;
  uint64_t io_in_progress = 0, io_ms = 0, weighted_io_ms = 0;
  uint64_t dc_completed = 0, dc_merged = 0, dc_sectors = 0, dc_ms = 0;
  bool has_discard = false;
};

struct DeviceInfo {
  std::string type;        // "SSD" | "HDD" | ""
  double      size_gb = 0; // 0 = unknown
};

static bool is_virtual_device(const std::string& dev) {
  return dev.starts_with("ram") || dev.starts_with("loop") ||
         dev.starts_with("zram") || dev.starts_with("fd") || dev == "sr0";
}

static std::vector<DiskStats> read_diskstats() {
  std::ifstream f("/proc/diskstats");
  if (!f.is_open()) return {};
  std::vector<DiskStats> stats;
  std::string line;
  while (std::getline(f, line)) {
    std::istringstream ss(line);
    DiskStats ds;
    int major = 0, minor = 0;
    if (!(ss >> major >> minor >> ds.device)) continue;
    ss >> ds.rd_completed >> ds.rd_merged >> ds.rd_sectors >> ds.rd_ms
       >> ds.wr_completed >> ds.wr_merged >> ds.wr_sectors >> ds.wr_ms
       >> ds.io_in_progress >> ds.io_ms >> ds.weighted_io_ms;
    if (ss >> ds.dc_completed)
      ss >> ds.dc_merged >> ds.dc_sectors >> ds.dc_ms;
    stats.push_back(std::move(ds));
  }
  std::erase_if(stats, [](const DiskStats& d) { return is_virtual_device(d.device); });
  return stats;
}

static DeviceInfo get_device_info(const std::string& dev) {
  DeviceInfo info;
  std::string base = "/sys/block/" + dev;
  if (!fs::exists(base)) return info;  // partition → unknown
  if (std::ifstream f(base + "/queue/rotational"); f) {
    std::string rot;
    if (f >> rot) info.type = (rot == "1") ? "HDD" : "SSD";
  }
  if (std::ifstream f(base + "/size"); f) {
    uint64_t sectors = 0;
    if (f >> sectors) info.size_gb = static_cast<double>(sectors) * 512.0 / 1e9;
  }
  return info;
}

static double read_uptime_s() {
  std::ifstream f("/proc/uptime");
  double up = 0, idle = 0;
  if (f >> up >> idle) return up;
  return 0;
}

// ── Formatting ────────────────────────────────────────────────────────────
static std::string fmt_bytes(double bytes) {
  const char* units[] = {"B", "K", "M", "G", "T"};
  int i = 0;
  while (bytes >= 1024.0 && i < 4) { bytes /= 1024.0; ++i; }
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.1f%s", bytes, units[i]);
  return buf;
}

static std::string fmt_count(uint64_t n) {
  char buf[32];
  unsigned long long v = static_cast<unsigned long long>(n);
  if (n >= 1'000'000'000ULL) std::snprintf(buf, sizeof buf, "%.2fG", n / 1e9);
  else if (n >= 1'000'000ULL) std::snprintf(buf, sizeof buf, "%.1fM", n / 1e6);
  else if (n >= 1'000ULL)     std::snprintf(buf, sizeof buf, "%.1fK", n / 1e3);
  else                        std::snprintf(buf, sizeof buf, "%llu", v);
  return buf;
}

static std::string fmt_size_gb(double gb) {
  if (gb <= 0) return "-";
  char buf[16];
  std::snprintf(buf, sizeof buf, "%.0fG", gb);
  return buf;
}

// ── Derived metrics ───────────────────────────────────────────────────────
struct Rates {
  double rps = 0, wps = 0;        // I/O operations per second
  double r_kb_s = 0, w_kb_s = 0;  // throughput KB/s
  double busy_pct = 0;            // % time with I/O in progress
  double avg_io_ms = 0;           // average I/O latency (ms)
};

static Rates delta_rates(const DiskStats& a, const DiskStats& b, double dt_s) {
  Rates r;
  if (dt_s <= 0) return r;
  uint64_t dr = b.rd_completed - a.rd_completed;
  uint64_t dw = b.wr_completed - a.wr_completed;
  uint64_t drs = b.rd_sectors - a.rd_sectors;
  uint64_t dws = b.wr_sectors - a.wr_sectors;
  uint64_t dio_ms = b.io_ms - a.io_ms;
  uint64_t dweight = b.weighted_io_ms - a.weighted_io_ms;
  uint64_t dios = dr + dw;
  r.rps = static_cast<double>(dr) / dt_s;
  r.wps = static_cast<double>(dw) / dt_s;
  r.r_kb_s = static_cast<double>(drs) * 512.0 / 1024.0 / dt_s;
  r.w_kb_s = static_cast<double>(dws) * 512.0 / 1024.0 / dt_s;
  r.busy_pct = std::min(100.0, static_cast<double>(dio_ms) / (dt_s * 1000.0) * 100.0);
  if (dios > 0) r.avg_io_ms = static_cast<double>(dweight) / static_cast<double>(dios);
  return r;
}

static Rates boot_rates(const DiskStats& s, double uptime_s) {
  Rates r;
  if (uptime_s <= 0) return r;
  uint64_t ios = s.rd_completed + s.wr_completed;
  r.rps = static_cast<double>(s.rd_completed) / uptime_s;
  r.wps = static_cast<double>(s.wr_completed) / uptime_s;
  r.r_kb_s = static_cast<double>(s.rd_sectors) * 512.0 / 1024.0 / uptime_s;
  r.w_kb_s = static_cast<double>(s.wr_sectors) * 512.0 / 1024.0 / uptime_s;
  r.busy_pct = std::min(100.0, static_cast<double>(s.io_ms) / (uptime_s * 1000.0) * 100.0);
  if (ios > 0) r.avg_io_ms = static_cast<double>(s.weighted_io_ms) / static_cast<double>(ios);
  return r;
}

// ── Rendering ─────────────────────────────────────────────────────────────
static void print_header() {
  char buf[160];
  std::snprintf(buf, sizeof buf,
    "%s%-8s %-4s %6s %8s %8s %8s %8s %7s %7s %6s %7s%s",
    CYAN.c_str(), "DEVICE", "TYPE", "SIZE", "READS", "WRITES",
    "r/s", "w/s", "rKB/s", "wKB/s", "%busy", "avgIO", RST.c_str());
  std::println(std::cout, "{}", buf);
}

static void print_row(const DiskStats& s, const DeviceInfo& info, const Rates& r) {
  std::string dev_color = info.type == "HDD" ? YEL : BOLD;
  std::string type = info.type.empty() ? "-" : info.type;
  std::string size = fmt_size_gb(info.size_gb);
  std::string busy = r.busy_pct > 80.0 ? RED : (r.busy_pct > 50.0 ? YEL : GREEN);
  std::string rcount = fmt_count(s.rd_completed);
  std::string wcount = fmt_count(s.wr_completed);
  char buf[320];
  std::snprintf(buf, sizeof buf,
    "%s%-8s%s %-4s %6s %8s %8s %8.1f %8.1f %7.0f %7.0f %s%5.1f%%%s %7.2f",
    dev_color.c_str(), s.device.c_str(), RST.c_str(), type.c_str(), size.c_str(),
    rcount.c_str(), wcount.c_str(),
    r.rps, r.wps, r.r_kb_s, r.w_kb_s,
    busy.c_str(), r.busy_pct, RST.c_str(), r.avg_io_ms);
  std::println(std::cout, "{}", buf);
}

static void print_table(const std::vector<DiskStats>& disks,
                        const std::vector<DeviceInfo>& infos,
                        const std::vector<Rates>& rates) {
  print_header();
  for (size_t i = 0; i < disks.size(); ++i)
    print_row(disks[i], infos[i], rates[i]);
}

// ── JSON output ───────────────────────────────────────────────────────────
static void print_json(const std::vector<DiskStats>& disks,
                       const std::vector<DeviceInfo>& infos,
                       const std::vector<Rates>& rates,
                       double uptime_s) {
  char buf[512];
  std::string out = "{\n";
  out += "  \"timestamp\": " + std::to_string(static_cast<uint64_t>(std::time(nullptr))) + ",\n";
  std::snprintf(buf, sizeof buf, "  \"uptime_s\": %.1f,\n", uptime_s);
  out += buf;
  out += "  \"disks\": [\n";
  uint64_t total_rb = 0, total_wb = 0;
  for (size_t i = 0; i < disks.size(); ++i) {
    const auto& s = disks[i];
    const auto& r = rates[i];
    uint64_t rb = s.rd_sectors * 512ULL, wb = s.wr_sectors * 512ULL;
    total_rb += rb; total_wb += wb;
    std::snprintf(buf, sizeof buf,
      "    {\"device\":\"%s\",\"type\":\"%s\",\"size_gb\":%.1f,"
      "\"reads\":%llu,\"writes\":%llu,\"read_bytes\":%llu,\"write_bytes\":%llu,"
      "\"rps\":%.1f,\"wps\":%.1f,\"r_kb_s\":%.1f,\"w_kb_s\":%.1f,"
      "\"busy_pct\":%.2f,\"avg_io_ms\":%.2f,\"io_in_progress\":%llu}%s\n",
      s.device.c_str(), (infos[i].type.empty() ? "-" : infos[i].type.c_str()),
      infos[i].size_gb,
      static_cast<unsigned long long>(s.rd_completed),
      static_cast<unsigned long long>(s.wr_completed),
      static_cast<unsigned long long>(rb),
      static_cast<unsigned long long>(wb),
      r.rps, r.wps, r.r_kb_s, r.w_kb_s, r.busy_pct, r.avg_io_ms,
      static_cast<unsigned long long>(s.io_in_progress),
      (i + 1 < disks.size() ? "," : ""));
    out += buf;
  }
  out += "  ],\n";
  std::snprintf(buf, sizeof buf,
    "  \"summary\": {\"disks\":%zu,\"total_read_gb\":%.1f,\"total_write_gb\":%.1f}\n",
    disks.size(), static_cast<double>(total_rb) / 1e9,
    static_cast<double>(total_wb) / 1e9);
  out += buf;
  out += "}\n";
  std::cout << out;
}

// ── Self tests ────────────────────────────────────────────────────────────
static void print_help();  // defined below (free function ordering)

static int g_pass = 0, g_fail = 0;
static void t_check(bool ok, const std::string& name) {
  if (ok) { ++g_pass; std::println(std::cout, "  {}ok{}  {}", GREEN, RST, name); }
  else    { ++g_fail; std::println(std::cout, "  {}FAIL{} {}", RED, RST, name); }
}

static int run_selftest() {
  std::println(std::cout, "diskstats --selftest");

  auto disks = read_diskstats();
  t_check(!disks.empty(), "parsea /proc/diskstats real (no vacio)");
  bool has_sda = std::any_of(disks.begin(), disks.end(),
                             [](const auto& d) { return d.device == "sda"; });
  t_check(has_sda, "dispositivo sda presente");
  bool no_virtual = std::none_of(disks.begin(), disks.end(),
                                 [](const DiskStats& d) { return is_virtual_device(d.device); });
  t_check(no_virtual, "dispositivos virtuales filtrados (loop/ram/sr0/zram)");

  t_check(100ULL * 512ULL == 51200ULL, "sectores->bytes: 100 sectores = 51200 B");
  t_check(fmt_bytes(51200.0) == "50.0K", "fmt_bytes auto-escala 51200 -> 50.0K");
  t_check(fmt_bytes(2.5e9) == "2.3G", "fmt_bytes auto-escala 2.5e9 -> 2.3G");
  t_check(fmt_size_gb(0) == "-", "fmt_size_gb desconocido -> '-'");

  DiskStats a, b;
  a.rd_completed = 1000; a.rd_sectors = 4000; a.io_ms = 100; a.weighted_io_ms = 200;
  b.rd_completed = 1100; b.rd_sectors = 4040; b.io_ms = 600; b.weighted_io_ms = 700;
  auto r = delta_rates(a, b, 2.0);
  t_check(std::abs(r.rps - 50.0) < 1e-9, "IOPS delta: 100 lecturas / 2s = 50.0");
  t_check(std::abs(r.r_kb_s - 10.0) < 1e-9, "throughput delta: 40 sectores / 2s = 10.0 KB/s");
  t_check(std::abs(r.busy_pct - 25.0) < 1e-9, "busy% delta: 500ms / 2000ms = 25.0%");
  t_check(std::abs(r.avg_io_ms - 5.0) < 1e-9, "latencia media: 500ms / 100 ops = 5.0ms");

  b.io_ms = 4500;  // 4400ms of IO in a 2000ms window
  auto r2 = delta_rates(a, b, 2.0);
  t_check(std::abs(r2.busy_pct - 100.0) < 1e-9, "busy% clamp: 4400ms/2000ms -> 100.0% (no 220)");

  DiskStats idle;
  auto r3 = delta_rates(idle, idle, 1.0);
  t_check(std::abs(r3.avg_io_ms) < 1e-9 && std::isfinite(r3.avg_io_ms),
          "latencia media con 0 ops = 0.00 (no NaN)");

  t_check(get_device_info("sda").size_gb > 0, "info /sys/block/sda: tamano > 0");
  double up = read_uptime_s();
  t_check(up > 0, "uptime /proc/uptime > 0");

  // JSON structural validation: balanced braces + quoted keys
  std::vector<DeviceInfo> jinfos;
  std::vector<Rates> jrates;
  for (const auto& d : disks) {
    jinfos.push_back(get_device_info(d.device));
    jrates.push_back(boot_rates(d, up));
  }
  std::string json;
  {
    std::ostringstream captured;
    auto* old = std::cout.rdbuf(captured.rdbuf());
    print_json(disks, jinfos, jrates, up);
    std::cout.rdbuf(old);
    json = captured.str();
  }
  int depth = 0; bool ok_braces = true;
  bool in_str = false, esc = false;
  for (char ch : json) {
    if (in_str) {
      if (esc) esc = false;
      else if (ch == '\\') esc = true;
      else if (ch == '"') in_str = false;
      continue;
    }
    if (ch == '"') in_str = true;
    else if (ch == '{' || ch == '[') ++depth;
    else if (ch == '}' || ch == ']') { --depth; if (depth < 0) { ok_braces = false; break; } }
  }
  t_check(ok_braces && depth == 0, "JSON: llaves/corchetes balanceados");
  t_check(json.find("\"device\":\"") != std::string::npos &&
          json.find("\"summary\"") != std::string::npos,
          "JSON: claves con comillas y seccion summary");

  // --help output contains usage
  std::string help_out;
  {
    std::ostringstream captured;
    auto* old = std::cout.rdbuf(captured.rdbuf());
    print_help();
    std::cout.rdbuf(old);
    help_out = captured.str();
  }
  t_check(help_out.find("Usage: ./diskstats") != std::string::npos, "--help documenta Usage");

  std::println(std::cout, "RESULT: {} passed, {} failed", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}

// ── CLI ───────────────────────────────────────────────────────────────────
static void print_help() {
  std::println(std::cout, "{}diskstats{} — Disk I/O monitor (Linux /proc/diskstats + /sys/block)",
               BOLD, RST);
  std::println(std::cout, "Build: g++ -std=c++23 -O2 -Wall -Wextra src/main.cpp -o diskstats");
  std::println(std::cout, "Usage: ./diskstats [options]");
  std::println(std::cout, "  -w, --watch N    refresh every N seconds (delta rates)");
  std::println(std::cout, "  -j, --json       JSON output (single snapshot, boot-averaged rates)");
  std::println(std::cout, "  -s, --sort KEY   sort: name|r|w|iops|busy (default name)");
  std::println(std::cout, "  -t, --top N      show only top N devices (with --sort)");
  std::println(std::cout, "  -c, --no-color   disable ANSI colors");
  std::println(std::cout, "  -h, --help       this help");
  std::println(std::cout, "      --selftest   run self-verification checks");
}

int main(int argc, char** argv) {
  bool watch = false, json = false, selftest = false;
  int interval = 2, top = 0;
  std::string sort_key = "name";

  for (int i = 1; i < argc; ++i) {
    std::string_view a{argv[i]};
    if ((a == "-w" || a == "--watch") && i + 1 < argc) { interval = std::stoi(argv[++i]); watch = true; }
    else if (a == "-j" || a == "--json") json = true;
    else if ((a == "-s" || a == "--sort") && i + 1 < argc) sort_key = argv[++i];
    else if ((a == "-t" || a == "--top") && i + 1 < argc) top = std::stoi(argv[++i]);
    else if (a == "-c" || a == "--no-color") g_color = false;
    else if (a == "-h" || a == "--help") { print_help(); return 0; }
    else if (a == "--selftest") selftest = true;
    else {
      std::println(std::cerr, "error: unknown option {}", a);
      print_help();
      return 2;
    }
  }

  if (selftest) return run_selftest();

  double uptime = read_uptime_s();

  if (watch) {
    auto prev = read_diskstats();
    auto t0 = steady_clock::now();
    while (true) {
      std::this_thread::sleep_for(std::chrono::seconds(interval));
      auto now = read_diskstats();
      double dt = std::chrono::duration<double>(steady_clock::now() - t0).count();
      t0 = steady_clock::now();
      auto find_prev = [&prev](const std::string& dev) -> const DiskStats* {
        for (const auto& p : prev) if (p.device == dev) return &p;
        return nullptr;
      };
      std::vector<DeviceInfo> infos;
      std::vector<Rates> rates;
      for (const auto& d : now) {
        infos.push_back(get_device_info(d.device));
        const DiskStats* p = find_prev(d.device);
        rates.push_back(p ? delta_rates(*p, d, dt) : Rates{});
      }
      if (g_color) std::cout << "\x1b[2J\x1b[H";
      std::println(std::cout, "{}Disk I/O Monitor{} — /proc/diskstats  (rates: last {}s)",
                   BOLD, RST, interval);
      print_table(now, infos, rates);
      std::cout << std::flush;
      prev = std::move(now);
    }
  }

  auto disks = read_diskstats();
  std::vector<DeviceInfo> infos;
  std::vector<Rates> rates;
  for (const auto& d : disks) {
    infos.push_back(get_device_info(d.device));
    rates.push_back(boot_rates(d, uptime));
  }

  if (sort_key != "name") {
    auto key_idx = [&](size_t i) -> double {
      if (sort_key == "r") return rates[i].r_kb_s;
      if (sort_key == "w") return rates[i].w_kb_s;
      if (sort_key == "iops") return rates[i].rps + rates[i].wps;
      if (sort_key == "busy") return rates[i].busy_pct;
      return 0;
    };
    std::vector<size_t> idx(disks.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    std::sort(idx.begin(), idx.end(),
              [&](size_t x, size_t y) { return key_idx(x) > key_idx(y); });
    if (top > 0 && static_cast<size_t>(top) < idx.size()) idx.resize(top);
    std::vector<DiskStats> nd; std::vector<DeviceInfo> ni; std::vector<Rates> nr;
    for (size_t i : idx) { nd.push_back(disks[i]); ni.push_back(infos[i]); nr.push_back(rates[i]); }
    disks = std::move(nd); infos = std::move(ni); rates = std::move(nr);
  } else if (top > 0 && static_cast<size_t>(top) < disks.size()) {
    disks.resize(top); infos.resize(top); rates.resize(top);
  }

  if (json) {
    print_json(disks, infos, rates, uptime);
    return 0;
  }

  std::println(std::cout, "{}Disk I/O Monitor{} — /proc/diskstats  (rates: avg since boot, uptime {:.0f}s)",
               BOLD, RST, uptime);
  print_table(disks, infos, rates);
  std::println(std::cout, "{}devices: {}{}", DIM, disks.size(), RST);
  return 0;
}
