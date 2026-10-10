#!/usr/bin/env python3
"""Focused regression tests for the benchmark orchestration helpers."""

from __future__ import annotations

import json
import signal
import sys
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path
from unittest import mock

from benchmark import benchmark_order
from benchmark import capture_container
from benchmark import capture_environment
from benchmark import collect_stats
from benchmark import select_median_run
from benchmark import summarize_collector_failures
from benchmark import validate_resource_headroom
from benchmark import verify_server


class CounterbalancedOrderTests(unittest.TestCase):
    def test_five_servers_use_the_three_round_counterbalanced_design(self) -> None:
        servers = ["ours", "hkr04", "fastmcpp", "cxxmcp", "neumann"]
        orders = benchmark_order.counterbalanced_orders(servers, runs=3, seed=417)

        self.assertEqual(len(orders), 3)
        base = orders[0]
        self.assertEqual(orders[1], base[1:] + base[:1])
        self.assertEqual(
            orders[2],
            [base[4], base[3], base[0], base[2], base[1]],
        )
        for order in orders:
            self.assertEqual(set(order), set(servers))
            self.assertEqual(len(order), len(set(order)))
        for server in servers:
            positions = [order.index(server) for order in orders]
            self.assertEqual(len(positions), len(set(positions)))


class MedianRunTests(unittest.TestCase):
    def test_operation_rps_prefers_the_measured_operation_rate(self) -> None:
        summary = {
            "rates": {"operations": {"per_second": 321.5}},
            "http": {"rps": 999.0},
        }
        self.assertEqual(select_median_run.operation_rps(summary), 321.5)
        self.assertEqual(
            select_median_run.operation_rps({"http": {"rps": 123.25}}),
            123.25,
        )

    def test_median_summary_is_paired_with_the_same_runs_resources(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            results_dir = Path(temporary_directory)
            (results_dir / "k6").mkdir()
            rps_by_run = {1: 120.0, 2: 100.0, 3: 110.0}
            for run, rps in rps_by_run.items():
                self._write_json(
                    results_dir / f"k6_summary_run{run}.json",
                    {
                        "marker": f"summary-{run}",
                        "config": {
                            "mode": "measurement",
                            "expected_negotiated_protocol_version": "2025-03-26",
                            "eligibility_contract": select_median_run.ELIGIBILITY_CONTRACT,
                        },
                        "rates": {
                            "operations": {"count": 4, "per_second": rps}
                        },
                        "check_breakdown": {
                            check_name: {"passes": 1, "fails": 0}
                            for check_name in (
                                *select_median_run.OPERATION_SHAPE_CHECKS,
                                *select_median_run.OPERATION_CONTRACT_CHECKS,
                            )
                        },
                        "errors": {
                            "mcp": 0,
                            "http": 0,
                            "checks": 0,
                            "mcp_rate": 0,
                            "http_rate": 0,
                            "check_pass_rate": 1,
                        },
                    },
                )
                samples = [
                    {
                        "timestamp": f"run-{run}-start",
                        "cpu_percent": run * 10,
                        "mem_usage_bytes": run * 1_000,
                        "mem_limit_bytes": 2_000_000,
                        "net_io_rx": run * 10_000,
                        "net_io_tx": run * 20_000,
                    },
                    {
                        "timestamp": f"run-{run}-end",
                        "cpu_percent": run * 20,
                        "mem_usage_bytes": run * 2_000,
                        "mem_limit_bytes": 2_000_000,
                        "net_io_rx": run * 10_000 + 1_234,
                        "net_io_tx": run * 20_000 + 5_678,
                    },
                ]
                for prefix in (
                    "stats",
                    "redis_stats",
                    "api_stats",
                    "k6_stats",
                    "host_stats",
                ):
                    self._write_json(results_dir / f"{prefix}_run{run}.json", samples)
                    audit: dict[str, object] = {
                        "marker": f"{prefix}-audit-{run}",
                        "status": "complete",
                    }
                    if prefix == "host_stats":
                        audit["host"] = {
                            "network_interface_policy": (
                                select_median_run.HOST_NETWORK_POLICY
                            ),
                            "network_interfaces": ["eth0", "lo"],
                            "active_network_interfaces": ["eth0", "lo"],
                            "retired_network_interfaces": [],
                            "ignored_new_network_interfaces": [],
                        }
                    self._write_json(
                        results_dir / f"{prefix}_run{run}.audit.json",
                        audit,
                    )
                for prefix in ("preflight", "postflight"):
                    self._write_json(
                        results_dir / f"protocol_{prefix}_run{run}.json",
                        {
                            "eligibility_contract": select_median_run.ELIGIBILITY_CONTRACT,
                            "eligibility_valid": True,
                            "negotiated_protocol_version": "2025-03-26",
                            "supplemental_validation": {
                                "required": False,
                                "valid": False,
                            },
                        },
                    )
                self._write_json(
                    results_dir / f"resource_headroom_run{run}.json",
                    {"valid": True},
                )
                self._write_json(
                    results_dir / f"cgroup_run{run}.json",
                    {"validation": {"ok": True}},
                )
                self._write_json(
                    results_dir / "k6" / f"cgroup_run{run}.json",
                    {"validation": {"ok": True}},
                )
                self._write_json(
                    results_dir / f"container_inspect_run{run}.json",
                    {"marker": f"container-{run}"},
                )
                self._write_json(
                    results_dir / f"image_inspect_run{run}.json",
                    {"marker": f"image-{run}"},
                )
                self._write_json(
                    results_dir / "k6" / f"container_inspect_run{run}.json",
                    {"marker": f"k6-container-{run}"},
                )
                self._write_json(
                    results_dir / "k6" / f"image_inspect_run{run}.json",
                    {"marker": f"k6-image-{run}"},
                )

            with mock.patch.object(
                sys,
                "argv",
                ["select_median_run.py", str(results_dir), "3"],
            ):
                self.assertEqual(select_median_run.main(), 0)

            self.assertEqual(
                self._read_json(results_dir / "k6_summary.json")["marker"],
                "summary-3",
            )
            self.assertEqual(
                self._read_json(results_dir / "stats.json")[0]["timestamp"],
                "run-3-start",
            )
            self.assertEqual(
                self._read_json(results_dir / "container_inspect.json")["marker"],
                "container-3",
            )
            self.assertEqual(
                self._read_json(results_dir / "image_inspect.json")["marker"],
                "image-3",
            )

            resource_summary = self._read_json(results_dir / "resource_summary.json")
            self.assertEqual(resource_summary["selected_run"], 3)
            self.assertEqual(
                resource_summary["server"]["network_bytes_during_collection"],
                {"rx": 1_234, "tx": 5_678},
            )
            self.assertEqual(resource_summary["server"]["cpu_percent"]["mean"], 45.0)
            self.assertEqual(
                resource_summary["host"]["network_observation"],
                {
                    "policy": select_median_run.HOST_NETWORK_POLICY,
                    "scope": "baseline_interface_cohort",
                    "coverage": "complete",
                    "observed_bytes_during_collection": {
                        "rx": 1_234,
                        "tx": 5_678,
                    },
                    "initial_interfaces": ["eth0", "lo"],
                    "active_interfaces_at_end": ["eth0", "lo"],
                    "retired_interfaces": [],
                    "ignored_new_interfaces": [],
                },
            )

            multi_run = self._read_json(results_dir / "k6_multi_run_stats.json")
            self.assertEqual(multi_run["median_run"], 3)
            self.assertEqual(multi_run["median_rps"], 110.0)
            self.assertEqual(
                multi_run["negotiated_protocol_version"], "2025-03-26"
            )

    def test_measurement_rejects_operations_not_reaching_contract_checks(self) -> None:
        check_breakdown = {
            check_name: {"passes": 1, "fails": 0}
            for check_name in (
                *select_median_run.OPERATION_SHAPE_CHECKS,
                *select_median_run.OPERATION_CONTRACT_CHECKS,
            )
        }
        summary = {
            "config": {
                "mode": "measurement",
                "eligibility_contract": select_median_run.ELIGIBILITY_CONTRACT,
            },
            "rates": {"operations": {"count": 5, "per_second": 1.0}},
            "check_breakdown": check_breakdown,
            "errors": {
                "mcp": 0,
                "http": 0,
                "checks": 0,
                "mcp_rate": 0,
                "http_rate": 0,
                "check_pass_rate": 1,
            },
        }

        with self.assertRaisesRegex(ValueError, "only 4 reached the required checks"):
            select_median_run.validate_measurement_summary(summary, Path("summary.json"))

    @staticmethod
    def _write_json(path: Path, value: object) -> None:
        path.write_text(json.dumps(value), encoding="utf-8")

    @staticmethod
    def _read_json(path: Path) -> object:
        return json.loads(path.read_text(encoding="utf-8"))


class CollectorParserTests(unittest.TestCase):
    def test_docker_stats_units_and_fields_are_parsed(self) -> None:
        self.assertEqual(collect_stats.parse_size_to_bytes("1.5 GiB"), 1_610_612_736)
        self.assertEqual(collect_stats.parse_size_to_bytes("2 MB"), 2_000_000)
        self.assertEqual(
            collect_stats.parse_size_to_bytes("1E+03 MB"), 1_000_000_000
        )
        self.assertEqual(
            collect_stats.parse_size_to_bytes("2.5e-1 GiB"), 268_435_456
        )
        self.assertEqual(collect_stats.parse_size_to_bytes("--"), 0)
        self.assertEqual(collect_stats.parse_cpu_percent("12.75%"), 12.75)
        self.assertEqual(
            collect_stats.parse_mem_usage("512 MiB / 2 GiB"),
            (536_870_912, 2_147_483_648),
        )
        self.assertEqual(
            collect_stats.parse_net_io("1.25 MB / 640 kB"),
            (1_250_000, 640_000),
        )
        self.assertEqual(
            collect_stats.parse_net_io("221MB / 1e+03MB"),
            (221_000_000, 1_000_000_000),
        )

    def test_malformed_docker_stats_values_are_rejected(self) -> None:
        with self.assertRaises(ValueError):
            collect_stats.parse_size_to_bytes("forty-two")
        with self.assertRaises(ValueError):
            collect_stats.parse_size_to_bytes("1eMB")
        with self.assertRaises(ValueError):
            collect_stats.parse_size_to_bytes("-1 MB")
        with self.assertRaises(ValueError):
            collect_stats.parse_size_to_bytes("1e3XB")
        with self.assertRaises(ValueError):
            collect_stats.parse_mem_usage("1 GiB")
        with self.assertRaises(ValueError):
            collect_stats.parse_net_io("1 MB / 2 MB / 3 MB")

    def test_collector_keeps_list_output_and_writes_audit_sidecar(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            output = Path(temporary_directory) / "stats_run1.json"
            call_count = 0

            class FakeContainerStream:
                def __init__(self, _target: str) -> None:
                    pass

                def collect(self) -> dict[str, int | float | str]:
                    nonlocal call_count
                    call_count += 1
                    if call_count == 3:
                        collect_stats.handle_signal(signal.SIGTERM, None)
                    return {
                        "timestamp": f"2026-07-19T00:00:0{call_count}+00:00",
                        "cpu_percent": 10.0,
                        "mem_usage_bytes": 100,
                        "mem_limit_bytes": 1_000,
                        "net_io_rx": call_count,
                        "net_io_tx": call_count,
                    }

                def close(self) -> None:
                    pass

            with (
                mock.patch.object(
                    sys,
                    "argv",
                    ["collect_stats.py", "container", str(output), "1"],
                ),
                mock.patch.object(collect_stats, "ContainerStatsStream", FakeContainerStream),
                mock.patch.object(collect_stats.signal, "signal"),
            ):
                self.assertEqual(collect_stats.main(), 0)

            samples = json.loads(output.read_text(encoding="utf-8"))
            audit = json.loads(
                collect_stats.audit_path_for(output).read_text(encoding="utf-8")
            )
            self.assertIsInstance(samples, list)
            self.assertEqual(len(samples), 3)
            self.assertEqual(audit["status"], "complete")
            self.assertEqual(audit["attempt_count"], 3)
            self.assertEqual(audit["sample_count"], 3)
            self.assertEqual(audit["failure_count"], 0)
            self.assertEqual(audit["termination"]["signal"], signal.SIGTERM)
            self.assertEqual(audit["source"], "persistent docker stats stream")

    def test_host_main_serializes_network_scope_and_churn(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            output = Path(temporary_directory) / "host_stats_run1.json"

            class FakeHostCollector:
                def collect(self) -> dict[str, int | float | str]:
                    collect_stats.handle_signal(signal.SIGTERM, None)
                    return {
                        "timestamp": "2026-07-19T00:00:01+00:00",
                        "cpu_percent": 10.0,
                        "mem_usage_bytes": 100,
                        "mem_limit_bytes": 1_000,
                        "net_io_rx": 10,
                        "net_io_tx": 20,
                    }

                def audit_metadata(self) -> dict[str, object]:
                    return {
                        "cpu_affinity": [0],
                        "cpu_capacity_cores": 1,
                        "network_interfaces": ["eth0", "veth-old"],
                        "active_network_interfaces": ["eth0"],
                        "retired_network_interfaces": ["veth-old"],
                        "ignored_new_network_interfaces": ["veth-new"],
                        "network_interface_policy": collect_stats.HOST_NETWORK_POLICY,
                        "network_interface_accounting": "test accounting",
                    }

            with (
                mock.patch.object(
                    sys,
                    "argv",
                    ["collect_stats.py", "@host", str(output), "1"],
                ),
                mock.patch.object(collect_stats, "HostStatsCollector", FakeHostCollector),
                mock.patch.object(collect_stats, "wait_until"),
                mock.patch.object(collect_stats.signal, "signal"),
            ):
                self.assertEqual(collect_stats.main(), 0)

            audit = json.loads(
                collect_stats.audit_path_for(output).read_text(encoding="utf-8")
            )
            self.assertEqual(audit["status"], "complete")
            self.assertEqual(audit["failure_count"], 0)
            self.assertEqual(
                audit["host"]["network_interface_policy"],
                collect_stats.HOST_NETWORK_POLICY,
            )
            self.assertEqual(
                audit["host"]["retired_network_interfaces"], ["veth-old"]
            )
            self.assertEqual(
                audit["host"]["ignored_new_network_interfaces"], ["veth-new"]
            )

    def test_docker_stats_screen_controls_are_not_part_of_the_sample(self) -> None:
        line = "\x1b[J\x1b[H12.5%|512 MiB / 2 GiB|221MB / 1e+03MB\x1b[K"
        cleaned = collect_stats.ANSI_CONTROL.sub("", line).strip()
        sample = collect_stats.parse_container_stats_line(cleaned)

        self.assertEqual(sample["cpu_percent"], 12.5)
        self.assertEqual(sample["mem_usage_bytes"], 536_870_912)
        self.assertEqual(sample["net_io_rx"], 221_000_000)
        self.assertEqual(sample["net_io_tx"], 1_000_000_000)

    def test_host_collector_uses_affinity_cpu_memory_and_network_counters(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            proc_root = Path(temporary_directory)
            (proc_root / "net").mkdir()
            stat = proc_root / "stat"
            stat.write_text(
                "cpu  0 0 0 0 0 0 0 0\n"
                "cpu0 100 0 50 850 0 0 0 0\n"
                "cpu1 200 0 50 750 0 0 0 0\n",
                encoding="utf-8",
            )
            (proc_root / "meminfo").write_text(
                "MemTotal:       1000 kB\nMemAvailable:    400 kB\n",
                encoding="utf-8",
            )
            (proc_root / "net" / "dev").write_text(
                "Inter-| Receive | Transmit\n"
                " lo: 10 0 0 0 0 0 0 0 20 0 0 0 0 0 0 0\n"
                " eth0: 30 0 0 0 0 0 0 0 40 0 0 0 0 0 0 0\n",
                encoding="utf-8",
            )
            collector = collect_stats.HostStatsCollector(
                proc_root, frozenset({0, 1})
            )
            stat.write_text(
                "cpu  0 0 0 0 0 0 0 0\n"
                "cpu0 125 0 65 910 0 0 0 0\n"
                "cpu1 215 0 55 830 0 0 0 0\n",
                encoding="utf-8",
            )

            sample = collector.collect()

            self.assertAlmostEqual(float(sample["cpu_percent"]), 60.0)
            self.assertEqual(sample["mem_usage_bytes"], 600 * 1024)
            self.assertEqual(sample["mem_limit_bytes"], 1000 * 1024)
            self.assertEqual(sample["net_io_rx"], 40)
            self.assertEqual(sample["net_io_tx"], 60)

    def test_host_collector_retires_disappeared_and_ignores_new_interfaces(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            proc_root = Path(temporary_directory)
            (proc_root / "net").mkdir()
            stat = proc_root / "stat"
            stat.write_text("cpu0 100 0 50 850 0 0 0 0\n", encoding="utf-8")
            (proc_root / "meminfo").write_text(
                "MemTotal: 1000 kB\nMemAvailable: 400 kB\n",
                encoding="utf-8",
            )
            network = proc_root / "net" / "dev"
            network.write_text(
                "Inter-| Receive | Transmit\n"
                " lo: 10 0 0 0 0 0 0 0 20 0 0 0 0 0 0 0\n"
                " eth0: 30 0 0 0 0 0 0 0 40 0 0 0 0 0 0 0\n"
                " veth-old: 500 0 0 0 0 0 0 0 600 0 0 0 0 0 0 0\n",
                encoding="utf-8",
            )
            collector = collect_stats.HostStatsCollector(
                proc_root, frozenset({0})
            )
            stat.write_text("cpu0 110 0 55 885 0 0 0 0\n", encoding="utf-8")
            network.write_text(
                "Inter-| Receive | Transmit\n"
                " lo: 12 0 0 0 0 0 0 0 23 0 0 0 0 0 0 0\n"
                " eth0: 35 0 0 0 0 0 0 0 45 0 0 0 0 0 0 0\n"
                " veth-new: 1000 0 0 0 0 0 0 0 2000 0 0 0 0 0 0 0\n",
                encoding="utf-8",
            )

            sample = collector.collect()

            self.assertEqual(sample["net_io_rx"], 547)
            self.assertEqual(sample["net_io_tx"], 668)
            self.assertEqual(
                collector.network_interfaces,
                frozenset({"lo", "eth0", "veth-old"}),
            )
            self.assertEqual(collector.active_network_interfaces, {"lo", "eth0"})
            self.assertEqual(collector.retired_network_interfaces, {"veth-old"})

            # Reusing the retired name with lower counters must not create a
            # false reset or contaminate the original baseline cohort.
            stat.write_text("cpu0 120 0 60 920 0 0 0 0\n", encoding="utf-8")
            network.write_text(
                "Inter-| Receive | Transmit\n"
                " lo: 14 0 0 0 0 0 0 0 26 0 0 0 0 0 0 0\n"
                " eth0: 38 0 0 0 0 0 0 0 48 0 0 0 0 0 0 0\n"
                " veth-old: 1 0 0 0 0 0 0 0 2 0 0 0 0 0 0 0\n"
                " veth-new: 2000 0 0 0 0 0 0 0 3000 0 0 0 0 0 0 0\n",
                encoding="utf-8",
            )

            sample = collector.collect()

            self.assertEqual(sample["net_io_rx"], 552)
            self.assertEqual(sample["net_io_tx"], 674)
            self.assertEqual(collector.active_network_interfaces, {"lo", "eth0"})
            self.assertEqual(collector.retired_network_interfaces, {"veth-old"})
            self.assertEqual(
                collector.ignored_new_network_interfaces,
                {"veth-new"},
            )
            self.assertEqual(
                collector.audit_metadata()["retired_network_interfaces"],
                ["veth-old"],
            )
            self.assertEqual(
                collector.audit_metadata()["ignored_new_network_interfaces"],
                ["veth-new"],
            )
            self.assertEqual(
                collector.audit_metadata()["network_interface_policy"],
                collect_stats.HOST_NETWORK_POLICY,
            )
            self.assertIn(
                "freeze last counters",
                collector.audit_metadata()["network_interface_accounting"],
            )

    def test_host_collector_rejects_resets_on_selected_interfaces(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            proc_root = Path(temporary_directory)
            (proc_root / "net").mkdir()
            stat = proc_root / "stat"
            stat.write_text("cpu0 100 0 50 850 0 0 0 0\n", encoding="utf-8")
            (proc_root / "meminfo").write_text(
                "MemTotal: 1000 kB\nMemAvailable: 400 kB\n",
                encoding="utf-8",
            )
            network = proc_root / "net" / "dev"
            network.write_text(
                "Inter-| Receive | Transmit\n"
                " lo: 10 0 0 0 0 0 0 0 20 0 0 0 0 0 0 0\n"
                " eth0: 30 0 0 0 0 0 0 0 40 0 0 0 0 0 0 0\n",
                encoding="utf-8",
            )
            collector = collect_stats.HostStatsCollector(
                proc_root, frozenset({0})
            )
            stat.write_text("cpu0 120 0 60 920 0 0 0 0\n", encoding="utf-8")
            network.write_text(
                "Inter-| Receive | Transmit\n"
                " lo: 9 0 0 0 0 0 0 0 24 0 0 0 0 0 0 0\n"
                " eth0: 36 0 0 0 0 0 0 0 46 0 0 0 0 0 0 0\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(RuntimeError, "counters reset"):
                collector.collect()

    def test_host_collector_fails_if_every_baseline_interface_retires(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            proc_root = Path(temporary_directory)
            (proc_root / "net").mkdir()
            stat = proc_root / "stat"
            stat.write_text("cpu0 100 0 50 850 0 0 0 0\n", encoding="utf-8")
            (proc_root / "meminfo").write_text(
                "MemTotal: 1000 kB\nMemAvailable: 400 kB\n",
                encoding="utf-8",
            )
            network = proc_root / "net" / "dev"
            network.write_text(
                "Inter-| Receive | Transmit\n"
                " old0: 10 0 0 0 0 0 0 0 20 0 0 0 0 0 0 0\n",
                encoding="utf-8",
            )
            collector = collect_stats.HostStatsCollector(
                proc_root, frozenset({0})
            )
            stat.write_text("cpu0 110 0 55 885 0 0 0 0\n", encoding="utf-8")
            network.write_text(
                "Inter-| Receive | Transmit\n"
                " new0: 30 0 0 0 0 0 0 0 40 0 0 0 0 0 0 0\n",
                encoding="utf-8",
            )

            with self.assertRaisesRegex(
                RuntimeError,
                "all baseline host network interfaces disappeared",
            ):
                collector.collect()


class CollectorFailureSummaryTests(unittest.TestCase):
    def test_failed_audit_is_used_instead_of_a_later_empty_log(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            results_dir = Path(temporary_directory)
            api_audit = results_dir / "api_stats_run1.audit.json"
            host_audit = results_dir / "host_stats_run1.audit.json"
            api_audit.write_text(
                json.dumps(
                    {
                        "status": "failed",
                        "target": "mcp-api-service",
                        "failures": [
                            {"error": "Unable to parse size value: '1e+03MB'"}
                        ],
                    }
                ),
                encoding="utf-8",
            )
            host_audit.write_text(
                json.dumps({"status": "complete", "target": "@host"}),
                encoding="utf-8",
            )

            summary = summarize_collector_failures.summarize_failures(
                [api_audit, host_audit]
            )

            self.assertEqual(
                summary,
                "mcp-api-service: Unable to parse size value: '1e+03MB'",
            )

    def test_missing_audit_is_reported(self) -> None:
        missing = Path("missing_stats_run1.audit.json")

        summary = summarize_collector_failures.summarize_failures([missing])

        self.assertIn("missing_stats_run1.audit.json: unavailable audit", summary)


class ResourceHeadroomTests(unittest.TestCase):
    MEMORY_LIMIT = 1_000

    def samples(
        self,
        *,
        count: int = 5,
        step_seconds: float = 1.0,
        cpu_percent: float = 25.0,
        memory_limit: int = MEMORY_LIMIT,
    ) -> list[dict[str, int | float | str]]:
        start = datetime(2026, 7, 19, tzinfo=timezone.utc)
        return [
            {
                "timestamp": (start + timedelta(seconds=index * step_seconds)).isoformat(),
                "cpu_percent": cpu_percent,
                "mem_usage_bytes": 250,
                "mem_limit_bytes": memory_limit,
                "net_io_rx": index,
                "net_io_tx": index,
            }
            for index in range(count)
        ]

    @staticmethod
    def audit(sample_count: int = 5, **overrides: object) -> dict[str, object]:
        value: dict[str, object] = {
            "schema_version": 1,
            "status": "complete",
            "interval_seconds": 1.0,
            "elapsed_seconds": 5.0,
            "termination": {"reason": "signal", "signal": signal.SIGTERM},
            "attempt_count": sample_count,
            "sample_count": sample_count,
            "failure_count": 0,
            "failures": [],
            "fatal_error": None,
        }
        value.update(overrides)
        return value

    def evaluate(
        self,
        samples: list[dict[str, int | float | str]],
        audit: dict[str, object],
        *,
        enforce_headroom: bool = True,
    ) -> dict[str, object]:
        return validate_resource_headroom.evaluate(
            "resource",
            samples,
            audit,
            cpu_limit=1.0,
            memory_limit=self.MEMORY_LIMIT,
            threshold=0.90,
            expected_duration=5.0,
            enforce_headroom=enforce_headroom,
        )

    def test_failed_collection_sparse_coverage_and_limit_mismatch_are_rejected(self) -> None:
        failed = self.evaluate(
            self.samples(),
            self.audit(
                attempt_count=6,
                failure_count=1,
                status="failed",
            ),
        )
        sparse = self.evaluate(
            self.samples(step_seconds=2.0),
            self.audit(),
        )
        wrong_limit = self.evaluate(
            self.samples(memory_limit=999),
            self.audit(),
        )

        self.assertFalse(failed["valid"])
        self.assertIn("failed attempts", str(failed["reason"]))
        self.assertFalse(sparse["valid"])
        self.assertIn("maximum sample gap", str(sparse["reason"]))
        self.assertFalse(wrong_limit["valid"])
        self.assertIn("mem_limit_bytes=999", str(wrong_limit["reason"]))

    def test_saturated_observed_target_is_valid_but_shared_resource_is_not(self) -> None:
        samples = self.samples(cpu_percent=95.0)
        audit = self.audit()

        shared = self.evaluate(samples, audit, enforce_headroom=True)
        observed = self.evaluate(samples, audit, enforce_headroom=False)

        self.assertFalse(shared["valid"])
        self.assertTrue(shared["headroom_exceeded"])
        self.assertTrue(observed["valid"])
        self.assertTrue(observed["headroom_exceeded"])
        self.assertFalse(observed["headroom_enforced"])

    def test_duration_parser_accepts_seconds_and_k6_style_suffixes(self) -> None:
        self.assertEqual(validate_resource_headroom.parse_duration("300"), 300.0)
        self.assertEqual(validate_resource_headroom.parse_duration("300s"), 300.0)
        self.assertEqual(validate_resource_headroom.parse_duration("5m"), 300.0)

    def test_decreasing_network_counters_are_rejected(self) -> None:
        samples = self.samples()
        samples[3]["net_io_rx"] = 1

        result = self.evaluate(samples, self.audit())

        self.assertFalse(result["valid"])
        self.assertIn("network counters decreased", str(result["reason"]))

    def test_resource_summary_does_not_mask_decreasing_network_counters(self) -> None:
        samples = self.samples()
        samples[3]["net_io_tx"] = 1

        with self.assertRaisesRegex(ValueError, "not monotonic"):
            select_median_run.resource_summary(samples, selected_run=1)

    def test_host_resource_summary_labels_partial_cohort_coverage(self) -> None:
        summary = select_median_run.resource_summary(
            self.samples(),
            selected_run=1,
            host_audit={
                "status": "complete",
                "host": {
                    "network_interface_policy": (
                        select_median_run.HOST_NETWORK_POLICY
                    ),
                    "network_interfaces": ["eth0", "veth-old"],
                    "active_network_interfaces": ["eth0"],
                    "retired_network_interfaces": ["veth-old"],
                    "ignored_new_network_interfaces": ["veth-new"],
                },
            },
        )

        self.assertNotIn("network_bytes_during_collection", summary)
        self.assertEqual(
            summary["network_observation"],
            {
                "policy": select_median_run.HOST_NETWORK_POLICY,
                "scope": "baseline_interface_cohort",
                "coverage": "partial",
                "observed_bytes_during_collection": {"rx": 4, "tx": 4},
                "initial_interfaces": ["eth0", "veth-old"],
                "active_interfaces_at_end": ["eth0"],
                "retired_interfaces": ["veth-old"],
                "ignored_new_interfaces": ["veth-new"],
            },
        )


class ProvenanceTests(unittest.TestCase):
    def test_tree_digest_tracks_sources_but_not_result_artifacts(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            project_dir = Path(temporary_directory)
            (project_dir / "include").mkdir()
            (project_dir / "benchmark" / "alternatives").mkdir(parents=True)
            (project_dir / "benchmark" / "results").mkdir(parents=True)
            (project_dir / "CMakeLists.txt").write_text("project(test)\n", encoding="utf-8")
            header = project_dir / "include" / "api.hpp"
            header.write_text("void api();\n", encoding="utf-8")
            adapter = project_dir / "benchmark" / "alternatives" / "adapter.cpp"
            adapter.write_text("int adapter();\n", encoding="utf-8")
            result = project_dir / "benchmark" / "results" / "summary.json"
            result.write_text("{}\n", encoding="utf-8")

            initial_digest, initial_paths = capture_environment.tree_digest(project_dir)
            self.assertEqual(len(initial_paths), 3)

            result.write_text('{"changed": true}\n', encoding="utf-8")
            unchanged_digest, unchanged_paths = capture_environment.tree_digest(project_dir)
            self.assertEqual((unchanged_digest, unchanged_paths), (initial_digest, initial_paths))

            adapter.write_text("int changed_adapter();\n", encoding="utf-8")
            changed_digest, changed_paths = capture_environment.tree_digest(project_dir)
            self.assertEqual(changed_paths, initial_paths)
            self.assertNotEqual(changed_digest, initial_digest)

    def test_source_snapshot_is_reconstructible_and_change_check_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            project_dir = Path(temporary_directory)
            (project_dir / "include").mkdir()
            (project_dir / "benchmark" / "results").mkdir(parents=True)
            (project_dir / "CMakeLists.txt").write_text("project(test)\n", encoding="utf-8")
            source = project_dir / "include" / "api.hpp"
            source.write_text("void api();\n", encoding="utf-8")
            digest, paths = capture_environment.tree_digest(project_dir)
            snapshot = project_dir / "benchmark" / "results" / "source_snapshot.tar"
            snapshot_digest = capture_environment.create_source_snapshot(
                project_dir, paths, snapshot
            )
            self.assertEqual(len(snapshot_digest), 64)
            environment = project_dir / "benchmark" / "results" / "environment.json"
            environment.write_text(
                json.dumps({"source": {"tree_sha256": digest}}), encoding="utf-8"
            )
            self.assertEqual(
                capture_environment.verify_source_digest(environment, project_dir), 0
            )
            source.write_text("void changed();\n", encoding="utf-8")
            self.assertEqual(
                capture_environment.verify_source_digest(environment, project_dir), 1
            )

    def test_commented_alternative_manifest_header_is_accepted(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            manifest = root / "sources.tsv"
            manifest.write_text(
                "# id\trepository\tcommit\tbenchmark_status\n"
                "sdk\thttps://example.test/sdk.git\tabc123\tsupported\n",
                encoding="utf-8",
            )
            with mock.patch.object(
                capture_environment,
                "git_repository",
                return_value={"origin_url": "https://example.test/sdk.git"},
            ):
                repositories = capture_environment.alternative_repositories(
                    manifest, root
                )
            self.assertEqual(repositories[0]["id"], "sdk")
            self.assertTrue(repositories[0]["origin_matches_manifest"])


class ContainerContractTests(unittest.TestCase):
    def test_fractional_cpu_limit_is_validated_in_docker_and_cgroup(self) -> None:
        inspect = {
            "HostConfig": {
                "NanoCpus": 500_000_000,
                "Memory": 536_870_912,
                "CpusetCpus": "2-3",
            }
        }
        selected = lambda value: {"selected": {"value": value}, "attempts": []}
        cgroup = {
            "version": 2,
            "cpu": selected("50000 100000"),
            "memory": selected("536870912"),
            "cpuset_effective": selected("2,3"),
        }
        result = capture_container.validate_resources(
            inspect,
            cgroup,
            expected_cpus=0.5,
            expected_memory_bytes=536_870_912,
            expected_cpuset="2-3",
        )
        self.assertTrue(result["ok"])

    def test_relative_executable_is_resolved_from_container_workdir(self) -> None:
        commands: list[list[str]] = []

        def run(command: list[str]) -> mock.Mock:
            commands.append(command)
            if command[:3] == ["docker", "exec", "api"]:
                return mock.Mock(returncode=0, stdout="/app/./api-service\n", stderr="")
            if command[:3] == ["docker", "cp", "-L"]:
                Path(command[-1]).write_bytes(b"server executable")
                return mock.Mock(returncode=0, stdout="", stderr="")
            self.fail(f"unexpected command: {command!r}")

        with mock.patch.object(capture_container, "run_command", side_effect=run):
            provenance = capture_container.executable_provenance("api", "./api-service")

        self.assertEqual(provenance["resolved_path"], "/app/./api-service")
        self.assertIsNotNone(provenance["sha256"])
        self.assertIn("api:/app/./api-service", commands[1])


class ProtocolCorrectnessTests(unittest.TestCase):
    class _FakeResponse:
        def __init__(
            self,
            body: bytes = b"",
            headers: dict[str, str] | None = None,
            status: int = 200,
        ) -> None:
            self._body = body
            self.headers = headers if headers is not None else {
                "Content-Type": "application/json"
            }
            self.status = status

        def read(self) -> bytes:
            return self._body

        def getcode(self) -> int:
            return self.status

        def __enter__(self) -> "ProtocolCorrectnessTests._FakeResponse":
            return self

        def __exit__(self, *_: object) -> None:
            return None

    def test_inherited_contract_accepts_python_schema_and_rust_counter(self) -> None:
        python_checkout_tool = {
            "name": "checkout",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "user_id": {"type": "string"},
                    "items": {"type": "array", "items": {}},
                },
            },
        }
        rust_checkout = {
            "user_id": "user-00001",
            "status": "confirmed",
            "total": 24.63,
            "items_count": 2,
            "rate_limit_count": 295,
        }

        self.assertIsInstance(python_checkout_tool["inputSchema"], dict)
        self.assertFalse(verify_server.validate_tool_schema(python_checkout_tool))
        self.assertTrue(
            verify_server.valid_upstream_checkout(rust_checkout, "user-00001")
        )
        self.assertFalse(verify_server.valid_exact_checkout(rust_checkout, "rust"))

    def test_checkout_side_effect_gate_checks_all_three_redis_mutations(self) -> None:
        before = {
            "rate_limit_count": 0,
            "history_length": 20,
            "product_42_popularity": 294.0,
        }
        after = {
            "rate_limit_count": 1,
            "history_length": 21,
            "product_42_popularity": 295.0,
        }

        self.assertTrue(verify_server.valid_workload_side_effects(before, after))
        after["history_length"] = 20
        self.assertFalse(verify_server.valid_workload_side_effects(before, after))

    def test_schema_contract_allows_metadata_and_closed_objects(self) -> None:
        tool = {
            "name": "checkout",
            "inputSchema": {
                "type": "object",
                "additionalProperties": False,
                "properties": {
                    "user_id": {"type": "string"},
                    "items": {
                        "type": "array",
                        "items": {
                            "type": "object",
                            "additionalProperties": False,
                            "required": ["quantity", "product_id"],
                            "properties": {
                                "quantity": {"type": "integer"},
                                "product_id": {"type": "integer"},
                            },
                        },
                    },
                },
            },
        }

        self.assertTrue(verify_server.validate_tool_schema(tool))

    def test_schema_contract_rejects_unmodeled_narrowing_assertions(self) -> None:
        tool = {
            "name": "checkout",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "user_id": {"type": "string"},
                    "items": {
                        "type": "array",
                        "items": {
                            "type": "object",
                            "properties": {
                                "product_id": {
                                    "type": "integer",
                                    "maximum": 0,
                                },
                                "quantity": {"type": "integer"},
                            },
                            "required": ["product_id", "quantity"],
                        },
                    },
                },
            },
        }

        self.assertFalse(verify_server.validate_tool_schema(tool))

    def test_schema_contract_accepts_compatible_rust_style_schema(self) -> None:
        tool = {
            "name": "checkout",
            "inputSchema": {
                "$schema": "https://json-schema.org/draft/2020-12/schema",
                "type": "object",
                "$defs": {
                    "CheckoutItem": {
                        "type": "object",
                        "properties": {
                            "product_id": {
                                "type": "integer",
                                "format": "uint32",
                                "minimum": 0,
                            },
                            "quantity": {
                                "type": "integer",
                                "format": "uint32",
                                "minimum": 0,
                            },
                        },
                        "required": ["product_id", "quantity"],
                    }
                },
                "properties": {
                    "user_id": {"type": "string"},
                    "items": {
                        "type": "array",
                        "items": {"$ref": "#/$defs/CheckoutItem"},
                    },
                },
            },
        }

        self.assertTrue(verify_server.validate_tool_schema(tool))

    def test_schema_contract_rejects_bound_that_excludes_fixture(self) -> None:
        tool = {
            "name": "checkout",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "user_id": {"type": "string"},
                    "items": {
                        "type": "array",
                        "items": {
                            "type": "object",
                            "properties": {
                                "product_id": {"type": "integer", "minimum": 43},
                                "quantity": {"type": "integer"},
                            },
                            "required": ["product_id", "quantity"],
                        },
                    },
                },
            },
        }

        self.assertFalse(verify_server.validate_tool_schema(tool))

    def test_schema_contract_rejects_unknown_assertion(self) -> None:
        tool = {
            "name": "get_user_cart",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "user_id": {
                        "type": "string",
                        "x-requires-canonical-user": True,
                    }
                },
            },
        }

        self.assertFalse(verify_server.validate_tool_schema(tool))

    def test_schema_contract_rejects_unresolved_or_external_references(self) -> None:
        for reference in ("#/$defs/Missing", "https://example.com/item.json"):
            with self.subTest(reference=reference):
                tool = {
                    "name": "checkout",
                    "inputSchema": {
                        "type": "object",
                        "properties": {
                            "user_id": {"type": "string"},
                            "items": {
                                "type": "array",
                                "items": {"$ref": reference},
                            },
                        },
                    },
                }
                self.assertFalse(verify_server.validate_tool_schema(tool))

    def test_response_parser_accepts_json_and_sse(self) -> None:
        self.assertEqual(
            verify_server.parse_response(b'{"result": 1}', "application/json"),
            {"result": 1},
        )
        self.assertEqual(
            verify_server.parse_response(
                b"event: message\ndata: {\"result\": 2}\n\n",
                "text/event-stream",
            ),
            {"result": 2},
        )
        self.assertIsNone(verify_server.parse_response(b" \n", "application/json"))
        with self.assertRaises(RuntimeError):
            verify_server.parse_response(b"not an MCP response", "text/event-stream")
        with self.assertRaises(json.JSONDecodeError):
            verify_server.parse_response(
                b"data: {\"result\": 2}\n\n", "application/json"
            )
        with self.assertRaises(RuntimeError):
            verify_server.parse_response(b"[{\"result\": 1}]", "application/json")

    def test_negotiated_protocol_and_session_headers_propagate(self) -> None:
        requests: list[object] = []
        responses = iter(
            [
                self._FakeResponse(
                    b'{"jsonrpc":"2.0","id":1,"result":'
                    b'{"protocolVersion":"2025-03-26",'
                    b'"capabilities":{"tools":{}},'
                    b'"serverInfo":{"name":"benchmark","version":"1.0"}}}',
                    {
                        "Mcp-Session-Id": "session-123",
                        "Content-Type": "application/json",
                    },
                ),
                self._FakeResponse(headers={}, status=202),
                self._FakeResponse(b'{"jsonrpc":"2.0","id":2,"result":{"tools":[]}}'),
                self._FakeResponse(headers={}, status=204),
            ]
        )

        def fake_urlopen(request: object, timeout: int) -> object:
            requests.append(request)
            return next(responses)

        with mock.patch.object(
            verify_server.urllib.request,
            "urlopen",
            side_effect=fake_urlopen,
        ):
            session = verify_server.McpSession("http://benchmark.test/mcp")
            session.post(
                {"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}}
            )
            session.close()

        self.assertEqual(len(requests), 4)
        request_headers = [
            {key.lower(): value for key, value in request.header_items()}
            for request in requests
        ]
        self.assertNotIn("mcp-protocol-version", request_headers[0])
        for headers in request_headers[1:]:
            self.assertEqual(headers["mcp-protocol-version"], "2025-03-26")
            self.assertEqual(headers["mcp-session-id"], "session-123")
        self.assertEqual(requests[0].get_method(), "POST")
        self.assertEqual(requests[-1].get_method(), "DELETE")

        initialize_payload = json.loads(requests[0].data.decode("utf-8"))
        self.assertEqual(
            initialize_payload["params"]["protocolVersion"],
            verify_server.PROTOCOL_VERSION,
        )


if __name__ == "__main__":
    unittest.main()
