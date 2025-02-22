// SPDX-License-Identifier: Apache-2.0
/*
Copyright (C) 2024 The Falco Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

*/

#ifdef __linux__

#include <libsinsp/sinsp_int.h>
#include <libsinsp/metrics_collector.h>
#include <libsinsp/plugin_manager.h>
#include <cmath>
#include <sys/times.h>
#include <sys/stat.h>
#include <re2/re2.h>

static re2::RE2 s_libs_metrics_units_suffix_pre_prometheus_text_conversion("(_kb|_bytes|_mb|_perc|_percentage|_ratio|_ns|_ts|_sec|_total)", re2::RE2::POSIX);
static re2::RE2 s_libs_metrics_units_memory_suffix("(_kb|_bytes)", re2::RE2::POSIX);
static re2::RE2 s_libs_metrics_units_perc_suffix("(_perc)", re2::RE2::POSIX);

// For simplicity, needs to stay in sync w/ typedef enum metrics_v2_value_unit
// https://prometheus.io/docs/practices/naming/ or https://prometheus.io/docs/practices/naming/#base-units.
static const char *const metrics_unit_name_mappings_prometheus[] = {
	[METRIC_VALUE_UNIT_COUNT] = "total",
	[METRIC_VALUE_UNIT_RATIO] = "ratio",
	[METRIC_VALUE_UNIT_PERC] = "percentage",
	[METRIC_VALUE_UNIT_MEMORY_BYTES] = "bytes",
	[METRIC_VALUE_UNIT_MEMORY_KIBIBYTES] = "kibibytes",
	[METRIC_VALUE_UNIT_MEMORY_MEGABYTES] = "megabytes",
	[METRIC_VALUE_UNIT_TIME_NS] = "nanoseconds",
	[METRIC_VALUE_UNIT_TIME_S] = "seconds",
	[METRIC_VALUE_UNIT_TIME_NS_COUNT] = "nanoseconds_total",
	[METRIC_VALUE_UNIT_TIME_S_COUNT] = "seconds_total",
	[METRIC_VALUE_UNIT_TIME_TIMESTAMP_NS] = "timestamp_nanoseconds",
};

static_assert(sizeof(metrics_unit_name_mappings_prometheus) / sizeof(metrics_unit_name_mappings_prometheus[0]) == METRIC_VALUE_UNIT_MAX, "metrics_unit_name_mappings_prometheus array size does not match expected size");

// For simplicity, needs to stay in sync w/ typedef enum metrics_v2_metric_type
// https://github.com/prometheus/docs/blob/main/content/docs/instrumenting/exposition_formats.md
static const char *const metrics_metric_type_name_mappings_prometheus[] = {
	[METRIC_VALUE_METRIC_TYPE_MONOTONIC] = "counter",
	[METRIC_VALUE_METRIC_TYPE_NON_MONOTONIC_CURRENT] = "gauge",
};

namespace libs::metrics {

std::string metric_value_to_text(const metrics_v2& metric)
{
	std::string value_text;
	switch (metric.type)
	{
	case METRIC_VALUE_TYPE_U32:
		value_text = std::to_string(metric.value.u32);
		break;
	case METRIC_VALUE_TYPE_S32:
		value_text = std::to_string(metric.value.s32);
		break;
	case METRIC_VALUE_TYPE_U64:
		value_text = std::to_string(metric.value.u64);
		break;
	case METRIC_VALUE_TYPE_S64:
		value_text = std::to_string(metric.value.s64);
		break;
	case METRIC_VALUE_TYPE_D:
		value_text = std::to_string(metric.value.d);
		break;
	case METRIC_VALUE_TYPE_F:
		value_text = std::to_string(metric.value.f);
		break;
	case METRIC_VALUE_TYPE_I:
		value_text = std::to_string(metric.value.i);
		break;
	default:
		ASSERT(false);
		break;
	}
	return value_text;
}

std::string prometheus_sanitize_metric_name(const std::string& name, const RE2& invalid_chars = RE2("[^a-zA-Z0-9_:]"))
{
	// https://prometheus.io/docs/concepts/data_model/#metric-names-and-labels
	std::string sanitized_name = name;
	RE2::GlobalReplace(&sanitized_name, invalid_chars, "_");
	RE2::GlobalReplace(&sanitized_name, "_+", "_");
	// Ensure it starts with a letter or underscore (if empty after sanitizing, set to "_")
	if (sanitized_name.empty() || (!std::isalpha(sanitized_name.front()) && sanitized_name.front() != '_'))
	{
		sanitized_name = "_" + sanitized_name;
	}
	return sanitized_name;
}

std::string prometheus_qualifier(std::string_view prometheus_namespace, std::string_view prometheus_subsystem)
{
	std::string qualifier;
	if (!prometheus_namespace.empty())
	{
		qualifier += std::string(prometheus_namespace) + "_";
	}
	if (!prometheus_subsystem.empty())
	{
		qualifier += std::string(prometheus_subsystem) + "_";
	}
	return qualifier;
}


std::string prometheus_exposition_text(std::string_view metric_qualified_name, std::string_view metric_name, std::string_view metric_type_name, std::string_view metric_value, const std::map<std::string, std::string>& const_labels)
{
	std::string fqn = prometheus_sanitize_metric_name(std::string(metric_qualified_name));
	std::string prometheus_text = "# HELP " + fqn + " https://falco.org/docs/metrics/\n";
	prometheus_text += "# TYPE " + fqn + " " + std::string(metric_type_name) + "\n";
	prometheus_text += fqn;
	if (!const_labels.empty())
	{
		static const RE2 label_invalid_chars("[^a-zA-Z0-9_]");
		prometheus_text += "{";
		bool first_label = true;
		for (const auto& [key, value] : const_labels)
		{
			if (key.empty())
			{
				continue;
			}
			if (!first_label)
			{
				prometheus_text += ",";
			} else
			{
				first_label = false;
			}
			prometheus_text += prometheus_sanitize_metric_name(key, label_invalid_chars) + "=\"" + value + "\"";
		}
		prometheus_text += "} "; // the white space at the end is important!
	} else
	{
		prometheus_text += " "; // the white space at the end is important!
	}
	prometheus_text += std::string(metric_value);
	prometheus_text += "\n";
	return prometheus_text;
}

std::string metrics_converter::convert_metric_to_text(const metrics_v2& metric) const
{
	return std::string(metric.name) + " " + metric_value_to_text(metric) + "\n";
}

void metrics_converter::convert_metric_to_unit_convention(metrics_v2& /*metric*/) const
{
	// Default does nothing
}

void output_rule_metrics_converter::convert_metric_to_unit_convention(metrics_v2& metric) const
{
	if((metric.unit == METRIC_VALUE_UNIT_MEMORY_BYTES || metric.unit == METRIC_VALUE_UNIT_MEMORY_KIBIBYTES) &&
		(metric.type == METRIC_VALUE_TYPE_U32 || metric.type == METRIC_VALUE_TYPE_U64))
	{
		if(metric.type == METRIC_VALUE_TYPE_U32)
		{
			metric.value.d = libs::metrics::convert_memory(metric.unit, METRIC_VALUE_UNIT_MEMORY_MEGABYTES, metric.value.u32);
		}
		else if(metric.type == METRIC_VALUE_TYPE_U64)
		{
			metric.value.d = libs::metrics::convert_memory(metric.unit, METRIC_VALUE_UNIT_MEMORY_MEGABYTES, metric.value.u64);
		}
		std::string metric_name_str(metric.name);
		RE2::GlobalReplace(&metric_name_str, s_libs_metrics_units_memory_suffix, "_mb");
		strlcpy(metric.name, metric_name_str.c_str(), METRIC_NAME_MAX);
		metric.type = METRIC_VALUE_TYPE_D;
		metric.unit = METRIC_VALUE_UNIT_MEMORY_MEGABYTES;
	}
}

std::string prometheus_metrics_converter::convert_metric_to_text_prometheus(const metrics_v2& metric, std::string_view prometheus_namespace, std::string_view prometheus_subsystem, const std::map<std::string, std::string>& const_labels) const
{
	std::string prometheus_metric_name_fully_qualified = prometheus_qualifier(prometheus_namespace, prometheus_subsystem) + std::string(metric.name) + "_";
	// Remove native libs unit suffixes if applicable.
	RE2::GlobalReplace(&prometheus_metric_name_fully_qualified, s_libs_metrics_units_suffix_pre_prometheus_text_conversion, "");
	prometheus_metric_name_fully_qualified += std::string(metrics_unit_name_mappings_prometheus[metric.unit]);
	return prometheus_exposition_text(prometheus_metric_name_fully_qualified,
										metric.name,
										metrics_metric_type_name_mappings_prometheus[metric.metric_type],
										metric_value_to_text(metric),
										const_labels);
}

std::string prometheus_metrics_converter::convert_metric_to_text_prometheus(std::string_view metric_name, std::string_view prometheus_namespace, std::string_view prometheus_subsystem, const std::map<std::string, std::string>& const_labels) const
{
	return prometheus_exposition_text(prometheus_qualifier(prometheus_namespace, prometheus_subsystem) + std::string(metric_name) + "_info",
										metric_name,
										"gauge",
										"1",
										const_labels);
}

void prometheus_metrics_converter::convert_metric_to_unit_convention(metrics_v2& metric) const
{
	if((metric.unit == METRIC_VALUE_UNIT_MEMORY_BYTES || metric.unit == METRIC_VALUE_UNIT_MEMORY_KIBIBYTES) &&
		(metric.type == METRIC_VALUE_TYPE_U32 || metric.type == METRIC_VALUE_TYPE_U64))
	{
		if(metric.type == METRIC_VALUE_TYPE_U32)
		{
			metric.value.d = libs::metrics::convert_memory(metric.unit, METRIC_VALUE_UNIT_MEMORY_BYTES, metric.value.u32);
		}
		else if(metric.type == METRIC_VALUE_TYPE_U64)
		{
			metric.value.d = libs::metrics::convert_memory(metric.unit, METRIC_VALUE_UNIT_MEMORY_BYTES, metric.value.u64);
		}
		std::string metric_name_str(metric.name);
		RE2::GlobalReplace(&metric_name_str, s_libs_metrics_units_memory_suffix, "_bytes");
		strlcpy(metric.name, metric_name_str.c_str(), METRIC_NAME_MAX);
		metric.type = METRIC_VALUE_TYPE_D;
		metric.unit = METRIC_VALUE_UNIT_MEMORY_BYTES;
	}
	else if(metric.unit == METRIC_VALUE_UNIT_PERC && metric.type == METRIC_VALUE_TYPE_D)
	{
		metric.value.d = metric.value.d / 100.0;
		std::string metric_name_str(metric.name);
		RE2::GlobalReplace(&metric_name_str, s_libs_metrics_units_perc_suffix, "_ratio");
		strlcpy(metric.name, metric_name_str.c_str(), METRIC_NAME_MAX);
		metric.type = METRIC_VALUE_TYPE_D;
		metric.unit = METRIC_VALUE_UNIT_RATIO;
	}
}

void libs_resource_utilization::get_rss_vsz_pss_total_memory_and_open_fds()
{
	FILE* f;
	char filepath[512];
	char line[512];

	/*
	 * Get memory usage of the agent itself (referred to as calling process meaning /proc/self/)
	*/

	//  No need for scap_get_host_root since we look at the agents' own process, accessible from it's own pid namespace (if applicable)
	f = fopen("/proc/self/status", "r");
	if(!f)
	{
		return;
	}

	while(fgets(line, sizeof(line), f) != nullptr)
	{
		if(strncmp(line, "VmSize:", 7) == 0)
		{
			sscanf(line, "VmSize: %" SCNu32, &m_vsz);		/* memory size returned in kb */
		}
		else if(strncmp(line, "VmRSS:", 6) == 0)
		{
			sscanf(line, "VmRSS: %" SCNu32, &m_rss);		/* memory size returned in kb */
		}
	}
	fclose(f);

	//  No need for scap_get_host_root since we look at the agents' own process, accessible from it's own pid namespace (if applicable)
	f = fopen("/proc/self/smaps_rollup", "r");
	if(!f)
	{
		ASSERT(false);
		return;
	}

	while(fgets(line, sizeof(line), f) != NULL)
	{
		if(strncmp(line, "Pss:", 4) == 0)
		{
			sscanf(line, "Pss: %" SCNu32, &m_pss);		/* memory size returned in kb */
			break;
		}
	}
	fclose(f);

	/*
	 * Get total host memory usage
	*/

	// Using scap_get_host_root since we look at the memory usage of the underlying host
	snprintf(filepath, sizeof(filepath), "%s/proc/meminfo", scap_get_host_root());
	f = fopen(filepath, "r");
	if(!f)
	{
		ASSERT(false);
		return;
	}

	uint64_t mem_total, mem_free, mem_buff, mem_cache = 0;

	while(fgets(line, sizeof(line), f) != NULL)
	{
		if(strncmp(line, "MemTotal:", 9) == 0)
		{
			sscanf(line, "MemTotal: %" SCNu64, &mem_total);		/* memory size returned in kb */
		}
		else if(strncmp(line, "MemFree:", 8) == 0)
		{
			sscanf(line, "MemFree: %" SCNu64, &mem_free);		/* memory size returned in kb */
		}
		else if(strncmp(line, "Buffers:", 8) == 0)
		{
			sscanf(line, "Buffers: %" SCNu64, &mem_buff);		/* memory size returned in kb */
		}
		else if(strncmp(line, "Cached:", 7) == 0)
		{
			sscanf(line, "Cached: %" SCNu64, &mem_cache);		/* memory size returned in kb */
		}
	}
	fclose(f);
	m_host_memory_used = mem_total - mem_free - mem_buff - mem_cache;

	/*
	 * Get total number of allocated file descriptors (not all open files!)
	 * File descriptor is a data structure used by a program to get a handle on a file
	*/

	// Using scap_get_host_root since we look at the total open fds of the underlying host
	snprintf(filepath, sizeof(filepath), "%s/proc/sys/fs/file-nr", scap_get_host_root());
	f = fopen(filepath, "r");
	if(!f)
	{
		ASSERT(false);
		return;
	}
	int matched_fds = fscanf(f, "%" SCNu64, &m_host_open_fds);
	fclose(f);

	if (matched_fds != 1) {
		ASSERT(false);
		return;
	}
}

void libs_resource_utilization::get_cpu_usage_and_total_procs(double start_time)
{
	FILE* f;
	char filepath[512];
	char line[512];

	struct tms time;
	if (times (&time) == (clock_t) -1)
	{
		return;
	}

	/* Number of clock ticks per second, often referred to as USER_HZ / jiffies. */
	long hz = 100;
#ifdef _SC_CLK_TCK
	if ((hz = sysconf(_SC_CLK_TCK)) < 0)
	{
		ASSERT(false);
		hz = 100;
	}
#endif
	/* Current uptime of the host machine in seconds.
	 * /proc/uptime offers higher precision w/ 2 decimals.
	 */

	// Using scap_get_host_root since we look at the uptime of the underlying host
	snprintf(filepath, sizeof(filepath), "%s/proc/uptime", scap_get_host_root());
	f = fopen(filepath, "r");
	if(!f)
	{
		ASSERT(false);
		return;
	}

	double machine_uptime_sec = 0;
	int matched_uptime = fscanf(f, "%lf", &machine_uptime_sec);
	fclose(f);

	if (matched_uptime != 1) {
		ASSERT(false);
		return;
	}

	/*
	 * Get CPU usage of the agent itself (referred to as calling process meaning /proc/self/)
	*/

	/* Current utime is amount of processor time in user mode of calling process. Convert to seconds. */
	double user_sec = (double)time.tms_utime / hz;

	/* Current stime is amount of time the calling process has been scheduled in kernel mode. Convert to seconds. */
	double system_sec = (double)time.tms_stime / hz;


	/* CPU usage as percentage is computed by dividing the time the process uses the CPU by the
	 * currently elapsed time of the calling process. Compare to `ps` linux util. */
	double elapsed_sec = machine_uptime_sec - start_time;
	if (elapsed_sec > 0)
	{
		m_cpu_usage_perc = (double)100.0 * (user_sec + system_sec) / elapsed_sec;
		m_cpu_usage_perc = std::round(m_cpu_usage_perc * 10.0) / 10.0; // round to 1 decimal
	}

	/*
	 * Get total host CPU usage (all CPUs) as percentage and retrieve number of procs currently running.
	*/

	// Using scap_get_host_root since we look at the total CPU usage of the underlying host
	snprintf(filepath, sizeof(filepath), "%s/proc/stat", scap_get_host_root());
	f = fopen(filepath, "r");
	if(!f)
	{
		ASSERT(false);
		return;
	}

	/* Need only first 7 columns of /proc/stat cpu line */
	uint64_t user, nice, system, idle, iowait, irq, softirq = 0;
	while(fgets(line, sizeof(line), f) != NULL)
	{
		if(strncmp(line, "cpu ", 4) == 0)
		{
			/* Always first line in /proc/stat file, unit: jiffies */
			sscanf(line, "cpu %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64, &user, &nice, &system, &idle, &iowait, &irq, &softirq);
		}
		else if(strncmp(line, "procs_running ", 14) == 0)
		{
			sscanf(line, "procs_running %" SCNu32, &m_host_procs_running);
			break;
		}
	}
	fclose(f);
	auto sum = user + nice + system + idle + iowait + irq + softirq;
	if (sum > 0)
	{
		m_host_cpu_usage_perc = 100.0 - ((idle * 100.0) / sum);
		m_host_cpu_usage_perc = std::round(m_host_cpu_usage_perc * 10.0) / 10.0; // round to 1 decimal
	}
}

std::vector<metrics_v2> libs_resource_utilization::to_metrics()
{
	std::vector<metrics_v2> metrics;
	metrics.emplace_back(new_metric("cpu_usage_perc",
								 METRICS_V2_RESOURCE_UTILIZATION,
								 METRIC_VALUE_TYPE_D,
								 METRIC_VALUE_UNIT_PERC,
								 METRIC_VALUE_METRIC_TYPE_NON_MONOTONIC_CURRENT,
								 m_cpu_usage_perc));
	metrics.emplace_back(new_metric("memory_rss_kb",
								 METRICS_V2_RESOURCE_UTILIZATION,
								 METRIC_VALUE_TYPE_U32,
								 METRIC_VALUE_UNIT_MEMORY_KIBIBYTES,
								 METRIC_VALUE_METRIC_TYPE_NON_MONOTONIC_CURRENT,
								 m_rss));
	metrics.emplace_back(new_metric("memory_vsz_kb",
								 METRICS_V2_RESOURCE_UTILIZATION,
								 METRIC_VALUE_TYPE_U32,
								 METRIC_VALUE_UNIT_MEMORY_KIBIBYTES,
								 METRIC_VALUE_METRIC_TYPE_NON_MONOTONIC_CURRENT,
								 m_vsz));
	metrics.emplace_back(new_metric("memory_pss_kb",
								 METRICS_V2_RESOURCE_UTILIZATION,
								 METRIC_VALUE_TYPE_U32,
								 METRIC_VALUE_UNIT_MEMORY_KIBIBYTES,
								 METRIC_VALUE_METRIC_TYPE_NON_MONOTONIC_CURRENT,
								 m_pss));
	metrics.emplace_back(new_metric("container_memory_used_bytes",
								 METRICS_V2_RESOURCE_UTILIZATION,
								 METRIC_VALUE_TYPE_U64,
								 METRIC_VALUE_UNIT_MEMORY_BYTES,
								 METRIC_VALUE_METRIC_TYPE_NON_MONOTONIC_CURRENT,
								 m_container_memory_used));
	metrics.emplace_back(new_metric("host_cpu_usage_perc",
								 METRICS_V2_RESOURCE_UTILIZATION,
								 METRIC_VALUE_TYPE_D,
								 METRIC_VALUE_UNIT_PERC,
								 METRIC_VALUE_METRIC_TYPE_NON_MONOTONIC_CURRENT,
								 m_host_cpu_usage_perc));
	metrics.emplace_back(new_metric("host_memory_used_kb",
								 METRICS_V2_RESOURCE_UTILIZATION,
								 METRIC_VALUE_TYPE_U32,
								 METRIC_VALUE_UNIT_MEMORY_KIBIBYTES,
								 METRIC_VALUE_METRIC_TYPE_NON_MONOTONIC_CURRENT,
								 m_host_memory_used));
	metrics.emplace_back(new_metric("host_procs_running",
								 METRICS_V2_RESOURCE_UTILIZATION,
								 METRIC_VALUE_TYPE_U32,
								 METRIC_VALUE_UNIT_COUNT,
								 METRIC_VALUE_METRIC_TYPE_NON_MONOTONIC_CURRENT,
								 m_host_procs_running));
	metrics.emplace_back(new_metric("host_open_fds",
								 METRICS_V2_RESOURCE_UTILIZATION,
								 METRIC_VALUE_TYPE_U64,
								 METRIC_VALUE_UNIT_COUNT,
								 METRIC_VALUE_METRIC_TYPE_NON_MONOTONIC_CURRENT,
								 m_host_open_fds));

	return metrics;
}

void libs_resource_utilization::get_container_memory_used()
{
	/* In Kubernetes `container_memory_working_set_bytes` is the memory measure the OOM killer uses
	 * and values from `/sys/fs/cgroup/memory/memory.usage_in_bytes` are close enough.
	 *
	 * Please note that `kubectl top pod` numbers would reflect the sum of containers in a pod and
	 * typically libs clients (e.g. Falco) pods contain sidekick containers that use memory as well.
	 * This metric accounts only for the container with the security monitoring agent running.
	*/
	const char* filepath = getenv(SINSP_AGENT_CGROUP_MEM_PATH_ENV_VAR);
	if (filepath == nullptr)
	{
		// No need for scap_get_host_root since we look at the container pid namespace (if applicable)
		// Known collision for VM memory usage, but this default value is configurable
		filepath = "/sys/fs/cgroup/memory/memory.usage_in_bytes";
	}

	FILE* f = fopen(filepath, "r");
	if(!f)
	{
		return;
	}

	/* memory size returned in bytes */
	int fscanf_matched = fscanf(f, "%" SCNu64, &m_container_memory_used);
	if (fscanf_matched != 1)
	{
		m_container_memory_used = 0;
	}

	fclose(f);
}

libs_state_counters::libs_state_counters(const std::shared_ptr<sinsp_stats_v2>& sinsp_stats_v2, sinsp_thread_manager* thread_manager) : m_sinsp_stats_v2(sinsp_stats_v2), m_n_fds(0), m_n_threads(0) {
	if (thread_manager != nullptr)
	{
		m_n_threads = thread_manager->get_thread_count();
		threadinfo_map_t* threadtable = thread_manager->get_threads();
		if (threadtable != nullptr)
		{
			threadtable->loop([this] (sinsp_threadinfo& tinfo) {
				sinsp_fdtable* fdtable = tinfo.get_fd_table();
				if (fdtable != nullptr)
				{
					this->m_n_fds += fdtable->size();
				}
				return true;
			});
		}
	}
}

std::vector<metrics_v2> libs_state_counters::to_metrics()
{
	std::vector<metrics_v2> metrics;

	metrics.emplace_back(new_metric("n_threads",
				  METRICS_V2_STATE_COUNTERS,
				  METRIC_VALUE_TYPE_U64,
				  METRIC_VALUE_UNIT_COUNT,
				  METRIC_VALUE_METRIC_TYPE_NON_MONOTONIC_CURRENT,
				  m_n_threads));
	metrics.emplace_back(new_metric("n_fds",
					  METRICS_V2_STATE_COUNTERS,
					  METRIC_VALUE_TYPE_U64,
					  METRIC_VALUE_UNIT_COUNT,
					  METRIC_VALUE_METRIC_TYPE_NON_MONOTONIC_CURRENT,
					  m_n_fds));

	if (m_sinsp_stats_v2 == nullptr) {
		return metrics;
	}

	metrics.emplace_back(new_metric("n_noncached_fd_lookups",
					  METRICS_V2_STATE_COUNTERS,
					  METRIC_VALUE_TYPE_U64,
					  METRIC_VALUE_UNIT_COUNT,
					  METRIC_VALUE_METRIC_TYPE_MONOTONIC,
					  m_sinsp_stats_v2->m_n_noncached_fd_lookups));
	metrics.emplace_back(new_metric("n_cached_fd_lookups",
					  METRICS_V2_STATE_COUNTERS,
					  METRIC_VALUE_TYPE_U64,
					  METRIC_VALUE_UNIT_COUNT,
					  METRIC_VALUE_METRIC_TYPE_MONOTONIC,
					  m_sinsp_stats_v2->m_n_cached_fd_lookups));
	metrics.emplace_back(new_metric("n_failed_fd_lookups",
					  METRICS_V2_STATE_COUNTERS,
					  METRIC_VALUE_TYPE_U64,
					  METRIC_VALUE_UNIT_COUNT,
					  METRIC_VALUE_METRIC_TYPE_MONOTONIC,
					  m_sinsp_stats_v2->m_n_failed_fd_lookups));
	metrics.emplace_back(new_metric("n_added_fds",
					  METRICS_V2_STATE_COUNTERS,
					  METRIC_VALUE_TYPE_U64,
					  METRIC_VALUE_UNIT_COUNT,
					  METRIC_VALUE_METRIC_TYPE_MONOTONIC,
					  m_sinsp_stats_v2->m_n_added_fds));
	metrics.emplace_back(new_metric("n_removed_fds",
					  METRICS_V2_STATE_COUNTERS,
					  METRIC_VALUE_TYPE_U64,
					  METRIC_VALUE_UNIT_COUNT,
					  METRIC_VALUE_METRIC_TYPE_MONOTONIC,
					  m_sinsp_stats_v2->m_n_removed_fds));
	metrics.emplace_back(new_metric("n_stored_evts",
					  METRICS_V2_STATE_COUNTERS,
					  METRIC_VALUE_TYPE_U64,
					  METRIC_VALUE_UNIT_COUNT,
					  METRIC_VALUE_METRIC_TYPE_MONOTONIC,
					  m_sinsp_stats_v2->m_n_stored_evts));
	metrics.emplace_back(new_metric("n_store_evts_drops",
					  METRICS_V2_STATE_COUNTERS,
					  METRIC_VALUE_TYPE_U64,
					  METRIC_VALUE_UNIT_COUNT,
					  METRIC_VALUE_METRIC_TYPE_MONOTONIC,
					  m_sinsp_stats_v2->m_n_store_evts_drops));
	metrics.emplace_back(new_metric("n_retrieved_evts",
					  METRICS_V2_STATE_COUNTERS,
					  METRIC_VALUE_TYPE_U64,
					  METRIC_VALUE_UNIT_COUNT,
					  METRIC_VALUE_METRIC_TYPE_MONOTONIC,
					  m_sinsp_stats_v2->m_n_retrieved_evts));
	metrics.emplace_back(new_metric("n_retrieve_evts_drops",
					  METRICS_V2_STATE_COUNTERS,
					  METRIC_VALUE_TYPE_U64,
					  METRIC_VALUE_UNIT_COUNT,
					  METRIC_VALUE_METRIC_TYPE_MONOTONIC,
					  m_sinsp_stats_v2->m_n_retrieve_evts_drops));
	metrics.emplace_back(new_metric("n_noncached_thread_lookups",
					  METRICS_V2_STATE_COUNTERS,
					  METRIC_VALUE_TYPE_U64,
					  METRIC_VALUE_UNIT_COUNT,
					  METRIC_VALUE_METRIC_TYPE_MONOTONIC,
					  m_sinsp_stats_v2->m_n_noncached_thread_lookups));
	metrics.emplace_back(new_metric("n_cached_thread_lookups",
					  METRICS_V2_STATE_COUNTERS,
					  METRIC_VALUE_TYPE_U64,
					  METRIC_VALUE_UNIT_COUNT,
					  METRIC_VALUE_METRIC_TYPE_MONOTONIC,
					  m_sinsp_stats_v2->m_n_cached_thread_lookups));
	metrics.emplace_back(new_metric("n_failed_thread_lookups",
					  METRICS_V2_STATE_COUNTERS,
					  METRIC_VALUE_TYPE_U64,
					  METRIC_VALUE_UNIT_COUNT,
					  METRIC_VALUE_METRIC_TYPE_MONOTONIC,
					  m_sinsp_stats_v2->m_n_failed_thread_lookups));
	metrics.emplace_back(new_metric("n_added_threads",
					  METRICS_V2_STATE_COUNTERS,
					  METRIC_VALUE_TYPE_U64,
					  METRIC_VALUE_UNIT_COUNT,
					  METRIC_VALUE_METRIC_TYPE_MONOTONIC,
					  m_sinsp_stats_v2->m_n_added_threads));
	metrics.emplace_back(new_metric("n_removed_threads",
					  METRICS_V2_STATE_COUNTERS,
					  METRIC_VALUE_TYPE_U64,
					  METRIC_VALUE_UNIT_COUNT,
					  METRIC_VALUE_METRIC_TYPE_MONOTONIC,
					  m_sinsp_stats_v2->m_n_removed_threads));
	metrics.emplace_back(new_metric("n_drops_full_threadtable",
					  METRICS_V2_STATE_COUNTERS,
					  METRIC_VALUE_TYPE_U32,
					  METRIC_VALUE_UNIT_COUNT,
					  METRIC_VALUE_METRIC_TYPE_MONOTONIC,
					  m_sinsp_stats_v2->m_n_drops_full_threadtable));
	metrics.emplace_back(new_metric("n_missing_container_images",
					  METRICS_V2_STATE_COUNTERS,
					  METRIC_VALUE_TYPE_U32,
					  METRIC_VALUE_UNIT_COUNT,
					  METRIC_VALUE_METRIC_TYPE_NON_MONOTONIC_CURRENT,
					  m_sinsp_stats_v2->m_n_missing_container_images));
	metrics.emplace_back(new_metric("n_containers",
					  METRICS_V2_STATE_COUNTERS,
					  METRIC_VALUE_TYPE_U32,
					  METRIC_VALUE_UNIT_COUNT,
					  METRIC_VALUE_METRIC_TYPE_NON_MONOTONIC_CURRENT,
					  m_sinsp_stats_v2->m_n_containers));
	return metrics;
}

void libs_metrics_collector::snapshot()
{
	m_metrics.clear();
	if (!m_inspector)
	{
		return;
	}

	/*
	 * libscap metrics
	 */

	if((m_metrics_flags & METRICS_V2_KERNEL_COUNTERS) || (m_metrics_flags & METRICS_V2_LIBBPF_STATS) || (m_metrics_flags & METRICS_V2_KERNEL_COUNTERS_PER_CPU))
	{
		uint32_t nstats = 0;
		int32_t rc = 0;
		// libscap metrics: m_metrics_flags are pushed down from consumers' input,
		// libbpf stats only collected when ENGINE_FLAG_BPF_STATS_ENABLED aka `kernel.bpf_stats_enabled = 1`
		const metrics_v2* metrics_v2_scap_snapshot = m_inspector->get_capture_stats_v2(m_metrics_flags, &nstats, &rc);
		if (metrics_v2_scap_snapshot && nstats > 0 && rc == 0)
		{
			// Move existing scap metrics raw buffer into m_metrics vector
			m_metrics.assign(metrics_v2_scap_snapshot, metrics_v2_scap_snapshot + nstats);
		}
	}

	/*
	 * libsinsp metrics
	 */
	if((m_metrics_flags & METRICS_V2_RESOURCE_UTILIZATION))
	{
		const scap_agent_info* agent_info = m_inspector->get_agent_info();
		libs_resource_utilization resource_utilization(agent_info->start_time);
		std::vector<metrics_v2> ru_metrics = resource_utilization.to_metrics();
		m_metrics.insert(m_metrics.end(), ru_metrics.begin(), ru_metrics.end());
	}

	if((m_metrics_flags & METRICS_V2_STATE_COUNTERS))
	{
		libs_state_counters state_counters(m_sinsp_stats_v2, m_inspector->m_thread_manager.get());
		std::vector<metrics_v2> sc_metrics = state_counters.to_metrics();
		m_metrics.insert(m_metrics.end(), sc_metrics.begin(), sc_metrics.end());
	}

	/*
	* plugins metrics
	*/
	if(m_metrics_flags & METRICS_V2_PLUGINS)
	{
		for (auto& p : m_inspector->get_plugin_manager()->plugins())
		{
			std::vector<metrics_v2> plugin_metrics = p->get_metrics();
			m_metrics.insert(m_metrics.end(), plugin_metrics.begin(), plugin_metrics.end());
		}
	}
}

const std::vector<metrics_v2>& libs_metrics_collector::get_metrics() const
{
	return m_metrics;
}

std::vector<metrics_v2>& libs_metrics_collector::get_metrics()
{
	return m_metrics;
}

libs_metrics_collector::libs_metrics_collector(sinsp* inspector, uint32_t flags) :
	m_inspector(inspector),
	m_metrics_flags(flags)
{
	if (m_inspector != nullptr)
	{
		m_sinsp_stats_v2 = m_inspector->get_sinsp_stats_v2();
	}
	else
	{
		m_sinsp_stats_v2 = nullptr;
	}
}

} // namespace libs::metrics

#endif
