// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "util/cpu-info.h"

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

#include <sched.h>
#include <stdlib.h>
#include <unistd.h>
#include <algorithm>
#include <fstream>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem.hpp>
#include <sys/sysinfo.h>

#include "common/compiler-util.h"
#include "common/config.h"
#include "common/status.h"
#include "gen-cpp/Metrics_types.h"
#include "gutil/strings/substitute.h"
#include "util/pretty-printer.h"

#include "common/names.h"

using boost::algorithm::contains;
using boost::algorithm::trim;
namespace fs = boost::filesystem;
using std::max;

DECLARE_bool(abort_on_config_error);
DEFINE_int32(num_cores, 0, "(Advanced) If > 0, it sets the number of cores available to"
    " Impala. Setting it to 0 means Impala will use all available cores on the machine"
    " according to /proc/cpuinfo.");

namespace {
// Helper function to warn if a given file does not contain an expected string as its
// first line. If the file cannot be opened, no error is reported.
void WarnIfFileNotEqual(
    const string& filename, const string& expected, const string& warning_text) {
  ifstream file(filename);
  if (!file) return;
  string line;
  getline(file, line);
  if (line != expected) {
    LOG(ERROR) << "Expected " << expected << ", actual " << line << endl << warning_text;
  }
}
} // end anonymous namespace

namespace impala {

const int64_t CpuInfo::SSSE3;
const int64_t CpuInfo::SSE4_1;
const int64_t CpuInfo::SSE4_2;
const int64_t CpuInfo::POPCNT;
const int64_t CpuInfo::AVX;
const int64_t CpuInfo::AVX2;
const int64_t CpuInfo::PCLMULQDQ;

bool CpuInfo::initialized_ = false;
int64_t CpuInfo::hardware_flags_ = 0;
int64_t CpuInfo::original_hardware_flags_;
int64_t CpuInfo::cycles_per_ms_;
int CpuInfo::num_cores_ = 1;
int CpuInfo::max_num_cores_;
string CpuInfo::model_name_ = "unknown";
int CpuInfo::max_num_numa_nodes_;
unique_ptr<int[]> CpuInfo::core_to_numa_node_;
vector<vector<int>> CpuInfo::numa_node_to_cores_;
vector<int> CpuInfo::numa_node_core_idx_;

static struct {
  string name;
  int64_t flag;
} flag_mappings[] =
{
  { "ssse3",     CpuInfo::SSSE3 },
  { "sse4_1",    CpuInfo::SSE4_1 },
  { "sse4_2",    CpuInfo::SSE4_2 },
  { "popcnt",    CpuInfo::POPCNT },
  { "avx",       CpuInfo::AVX },
  { "avx2",      CpuInfo::AVX2 },
  { "pclmulqdq", CpuInfo::PCLMULQDQ }
};
static const long num_flags = sizeof(flag_mappings) / sizeof(flag_mappings[0]);

// Helper function to parse for hardware flags.
// values contains a list of space-seperated flags.  check to see if the flags we
// care about are present.
// Returns a bitmap of flags.
int64_t ParseCPUFlags(const string& values) {
  int64_t flags = 0;
  for (int i = 0; i < num_flags; ++i) {
    if (contains(values, flag_mappings[i].name)) {
      flags |= flag_mappings[i].flag;
    }
  }
  return flags;
}

void CpuInfo::Init() {
  string line;
  string name;
  string value;

  float max_mhz = 0;
  int num_cores = 0;

  // Read from /proc/cpuinfo
  ifstream cpuinfo("/proc/cpuinfo");
  while (cpuinfo) {
    getline(cpuinfo, line);
    size_t colon = line.find(':');
    if (colon != string::npos) {
      name = line.substr(0, colon - 1);
      value = line.substr(colon + 1, string::npos);
      trim(name);
      trim(value);
      if (name.compare("flags") == 0) {
        hardware_flags_ |= ParseCPUFlags(value);
      } else if (name.compare("cpu MHz") == 0) {
        // Every core will report a different speed.  We'll take the max, assuming
        // that when impala is running, the core will not be in a lower power state.
        // TODO: is there a more robust way to do this, such as
        // Window's QueryPerformanceFrequency()
        float mhz = atof(value.c_str());
        max_mhz = max(mhz, max_mhz);
      } else if (name.compare("processor") == 0) {
        ++num_cores;
      } else if (name.compare("model name") == 0) {
        model_name_ = value;
      }
    }
  }

  if (max_mhz != 0) {
    cycles_per_ms_ = max_mhz * 1000;
  } else {
    cycles_per_ms_ = 1000000;
  }
  original_hardware_flags_ = hardware_flags_;

  if (num_cores > 0) {
    num_cores_ = num_cores;
  } else {
    num_cores_ = 1;
  }
  if (FLAGS_num_cores > 0) num_cores_ = FLAGS_num_cores;
  max_num_cores_ = get_nprocs_conf();

  // Print a warning if something is wrong with sched_getcpu().
#ifdef HAVE_SCHED_GETCPU
  if (sched_getcpu() == -1) {
    LOG(WARNING) << "Kernel does not support getcpu(). Performance may be impacted.";
  }
#else
  LOG(WARNING) << "Built on a system without sched_getcpu() support. Performance may"
               << " be impacted.";
#endif

  InitNuma();
  initialized_ = true;
}

void CpuInfo::InitNuma() {
  // Use the NUMA info in the /sys filesystem. which is part of the Linux ABI:
  // see https://www.kernel.org/doc/Documentation/ABI/stable/sysfs-devices-node and
  // https://www.kernel.org/doc/Documentation/ABI/testing/sysfs-devices-system-cpu
  // The filesystem entries are only present if the kernel was compiled with NUMA support.
  core_to_numa_node_.reset(new int[max_num_cores_]);

  if (!fs::is_directory("/sys/devices/system/node")) {
    LOG(WARNING) << "/sys/devices/system/node is not present - no NUMA support";
    // Assume a single NUMA node.
    max_num_numa_nodes_ = 1;
    std::fill_n(core_to_numa_node_.get(), max_num_cores_, 0);
    InitNumaNodeToCores();
    return;
  }

  // Search for node subdirectories - node0, node1, node2, etc to determine possible
  // NUMA nodes.
  fs::directory_iterator dir_it("/sys/devices/system/node");
  max_num_numa_nodes_ = 0;
  for (; dir_it != fs::directory_iterator(); ++dir_it) {
    const string filename = dir_it->path().filename().string();
    if (filename.find("node") == 0) ++max_num_numa_nodes_;
  }
  if (max_num_numa_nodes_ == 0) {
    LOG(WARNING) << "Could not find nodes in /sys/devices/system/node";
    max_num_numa_nodes_ = 1;
  }

  // Check which NUMA node each core belongs to based on the existence of a symlink
  // to the node subdirectory.
  for (int core = 0; core < max_num_cores_; ++core) {
    bool found_numa_node = false;
    for (int node = 0; node < max_num_numa_nodes_; ++node) {
      if (fs::exists(Substitute("/sys/devices/system/cpu/cpu$0/node$1", core, node))) {
        core_to_numa_node_[core] = node;
        found_numa_node = true;
        break;
      }
    }
    if (!found_numa_node) {
      LOG(WARNING) << "Could not determine NUMA node for core " << core
                   << " from /sys/devices/system/cpu/";
      core_to_numa_node_[core] = 0;
    }
  }
  InitNumaNodeToCores();
}

void CpuInfo::InitFakeNumaForTest(
    int max_num_numa_nodes, const vector<int>& core_to_numa_node) {
  DCHECK_EQ(max_num_cores_, core_to_numa_node.size());
  max_num_numa_nodes_ = max_num_numa_nodes;
  for (int i = 0; i < max_num_cores_; ++i) {
    core_to_numa_node_[i] = core_to_numa_node[i];
  }
  numa_node_to_cores_.clear();
  InitNumaNodeToCores();
}

void CpuInfo::InitNumaNodeToCores() {
  DCHECK(numa_node_to_cores_.empty());
  numa_node_to_cores_.resize(max_num_numa_nodes_);
  numa_node_core_idx_.resize(max_num_cores_);
  for (int core = 0; core < max_num_cores_; ++core) {
    vector<int>* cores_of_node = &numa_node_to_cores_[core_to_numa_node_[core]];
    numa_node_core_idx_[core] = cores_of_node->size();
    cores_of_node->push_back(core);
  }
}

Status CpuInfo::EnforceCpuRequirements() {
  // This imposes a CPU requirement for x86_64. This function may later be modified
  // to impose a similar requirement for other platforms.
#ifdef __x86_64__
  if (!CpuInfo::IsSupported(CpuInfo::AVX2)) {
    return Status("This machine does not meet the minimum requirements for Impala "
                  "functionality. The CPU does not support AVX2 (Advanced Vector "
                  "Extensions 2).");
  }
#endif
  return Status::OK();
}

void CpuInfo::VerifyPerformanceGovernor() {
  for (int cpu_id = 0; cpu_id < CpuInfo::num_cores(); ++cpu_id) {
    const string governor_file =
        Substitute("/sys/devices/system/cpu/cpu$0/cpufreq/scaling_governor", cpu_id);
    const string warning_text = Substitute(
        "WARNING: CPU $0 is not using 'performance' governor. Note that changing the "
        "governor to 'performance' will reset the no_turbo setting to 0.",
        cpu_id);
    WarnIfFileNotEqual(governor_file, "performance", warning_text);
  }
}

void CpuInfo::VerifyTurboDisabled() {
  WarnIfFileNotEqual("/sys/devices/system/cpu/intel_pstate/no_turbo", "1",
      "WARNING: CPU turbo is enabled. This setting can change the clock frequency of CPU "
      "cores during the benchmark run, which can lead to inaccurate results. You can "
      "disable CPU turbo by writing a 1 to "
      "/sys/devices/system/cpu/intel_pstate/no_turbo. Note that changing the governor to "
      "'performance' will reset this to 0.");
}

void CpuInfo::EnableFeature(long flag, bool enable) {
  DCHECK(initialized_);
  if (!enable) {
    hardware_flags_ &= ~flag;
  } else {
    // Can't turn something on that can't be supported
    DCHECK((original_hardware_flags_ & flag) != 0);
    hardware_flags_ |= flag;
  }
}

int CpuInfo::GetCurrentCore() {
  // sched_getcpu() is not supported on some old kernels/glibcs (like the versions that
  // shipped with CentOS 5). In that case just pretend we're always running on CPU 0
  // so that we can build and run with degraded perf.
#ifdef HAVE_SCHED_GETCPU
  int cpu = sched_getcpu();
  // The syscall may not be supported even if the function exists.
  if (UNLIKELY(cpu < 0)) return 0;
  if (UNLIKELY(cpu >= max_num_cores_)) {
    // IMPALA-6595: on some systems it appears that sched_getcpu() can return
    // out-of-range CPU ids. We need to avoid returning bogus values from this function,
    // but should warn the user that something weird is happening.
    const int MAX_WARNINGS = 20;
    LOG_FIRST_N(WARNING, MAX_WARNINGS)
      << "sched_getcpu() returned an out-of-range CPU identifier '" << cpu << "'. "
      << "The OS originally reported a maximum of " << max_num_cores_ << " online cores. "
      << "Performance may be negatively affected. This may happen if virtualization "
      << "software incorrectly virtualizes certain instructions. See IMPALA-6595 for "
      << "more information. These warnings will stop after " << MAX_WARNINGS << " "
      << "occurrences.";
    return cpu % max_num_cores_;
  }
  return cpu;
#else
  return 0;
#endif
}

void CpuInfo::GetCacheInfo(long cache_sizes[NUM_CACHE_LEVELS],
      long cache_line_sizes[NUM_CACHE_LEVELS]) {
#ifdef __APPLE__
  // On Mac OS X use sysctl() to get the cache sizes
  size_t len = 0;
  sysctlbyname("hw.cachesize", NULL, &len, NULL, 0);
  uint64_t* data = static_cast<uint64_t*>(malloc(len));
  sysctlbyname("hw.cachesize", data, &len, NULL, 0);
  DCHECK(len / sizeof(uint64_t) >= 3);
  for (size_t i = 0; i < NUM_CACHE_LEVELS; ++i) {
    cache_sizes[i] = data[i];
  }
  size_t linesize;
  size_t sizeof_linesize = sizeof(linesize);
  sysctlbyname("hw.cachelinesize", &linesize, &sizeof_linesize, NULL, 0);
  for (size_t i = 0; i < NUM_CACHE_LEVELS; ++i) cache_line_sizes[i] = linesize;
#else
  // Call sysconf to query for the cache sizes
  // Note: on some systems (e.g. RHEL 5 on AWS EC2), this returns 0 instead of the
  // actual cache line size.
  cache_sizes[L1_CACHE] = sysconf(_SC_LEVEL1_DCACHE_SIZE);
  cache_sizes[L2_CACHE] = sysconf(_SC_LEVEL2_CACHE_SIZE);
  cache_sizes[L3_CACHE] = sysconf(_SC_LEVEL3_CACHE_SIZE);

  cache_line_sizes[L1_CACHE] = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
  cache_line_sizes[L2_CACHE] = sysconf(_SC_LEVEL2_CACHE_LINESIZE);
  cache_line_sizes[L3_CACHE] = sysconf(_SC_LEVEL3_CACHE_LINESIZE);
#endif
}

string CpuInfo::DebugString() {
  DCHECK(initialized_);
  stringstream stream;
  long cache_sizes[NUM_CACHE_LEVELS];
  long cache_line_sizes[NUM_CACHE_LEVELS];
  GetCacheInfo(cache_sizes, cache_line_sizes);

  string L1 = Substitute("L1 Cache: $0 (Line: $1)",
      PrettyPrinter::Print(cache_sizes[L1_CACHE], TUnit::BYTES),
      PrettyPrinter::Print(cache_line_sizes[L1_CACHE], TUnit::BYTES));
  string L2 = Substitute("L2 Cache: $0 (Line: $1)",
      PrettyPrinter::Print(cache_sizes[L2_CACHE], TUnit::BYTES),
      PrettyPrinter::Print(cache_line_sizes[L2_CACHE], TUnit::BYTES));
  string L3 = Substitute("L3 Cache: $0 (Line: $1)",
      PrettyPrinter::Print(cache_sizes[L3_CACHE], TUnit::BYTES),
      PrettyPrinter::Print(cache_line_sizes[L3_CACHE], TUnit::BYTES));
  stream << "Cpu Info:" << endl
         << "  Model: " << model_name_ << endl
         << "  Cores: " << num_cores_ << endl
         << "  Max Possible Cores: " << max_num_cores_ << endl
         << "  " << L1 << endl
         << "  " << L2 << endl
         << "  " << L3 << endl
         << "  Hardware Supports:" << endl;
  for (int i = 0; i < num_flags; ++i) {
    if (IsSupported(flag_mappings[i].flag)) {
      stream << "    " << flag_mappings[i].name << endl;
    }
  }
  stream << "  Numa Nodes: " << max_num_numa_nodes_ << endl;
  stream << "  Numa Nodes of Cores:";
  for (int core = 0; core < max_num_cores_; ++core) {
    stream << " " << core << "->" << core_to_numa_node_[core] << " |";
  }
  stream << endl;
  return stream.str();
}

}
