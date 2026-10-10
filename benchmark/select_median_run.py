#!/usr/bin/env python3
"""Select the median measured run and pair it with matching resource evidence."""

from __future__ import annotations

import json
import math
import shutil
import statistics
import sys
from pathlib import Path
from typing import Any


ELIGIBILITY_CONTRACT = "upstream-v2-strict-mcp-v1"
HOST_NETWORK_POLICY = "baseline-cohort-retire-v1"
OPERATION_SHAPE_CHECKS = (
    "search_products returns a valid tool result",
    "get_user_cart returns a valid tool result",
    "checkout returns a valid tool result",
    "tools/list returns a valid tool collection",
)
OPERATION_CONTRACT_CHECKS = (
    "search_products satisfies the benchmark contract",
    "get_user_cart satisfies the benchmark contract",
    "checkout satisfies the benchmark contract",
    "tools/list satisfies the benchmark contract",
)


def finite_number(value: Any, description: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{description} is not numeric")
    number = float(value)
    if not math.isfinite(number):
        raise ValueError(f"{description} is not finite")
    return number


def exact_nonnegative_integer(value: Any, description: str) -> int:
    number = finite_number(value, description)
    if number != int(number):
        raise ValueError(f"{description} is not an exact integer")
    parsed = int(number)
    if parsed < 0:
        raise ValueError(f"{description} is negative")
    return parsed


def operation_rps(summary: dict[str, Any]) -> float:
    rates = summary.get("rates", {})
    operations = rates.get("operations", {})
    if isinstance(operations, dict) and "per_second" in operations:
        return float(operations["per_second"])
    return float(summary.get("http", {}).get("rps", 0))


def validate_measurement_summary(summary: dict[str, Any], path: Path) -> None:
    if not isinstance(summary, dict):
        raise ValueError(f"{path} is not a JSON object")
    config = summary.get("config")
    if not isinstance(config, dict) or config.get("mode") != "measurement":
        raise ValueError(f"{path} is not a measurement summary")
    if config.get("eligibility_contract") != ELIGIBILITY_CONTRACT:
        raise ValueError(f"{path} used a different eligibility contract")
    rates = summary.get("rates")
    operations = rates.get("operations") if isinstance(rates, dict) else None
    if not isinstance(operations, dict) or "per_second" not in operations:
        raise ValueError(f"{path} has no complete benchmark-operation rate")
    rps = finite_number(operations["per_second"], f"{path} operation rate")
    if "count" not in operations:
        raise ValueError(f"{path} has no benchmark-operation count")
    operation_count = exact_nonnegative_integer(
        operations["count"], f"{path} operation count"
    )
    if operation_count <= 0:
        raise ValueError(f"{path} has no measured benchmark operations")
    if rps <= 0:
        raise ValueError(f"{path} has no measured benchmark operations")

    errors = summary.get("errors")
    required_error_fields = {
        "mcp",
        "http",
        "checks",
        "mcp_rate",
        "http_rate",
        "check_pass_rate",
    }
    if not isinstance(errors, dict) or not required_error_fields.issubset(errors):
        raise ValueError(f"{path} has incomplete correctness metrics")
    values = {
        name: finite_number(errors[name], f"{path} errors.{name}")
        for name in required_error_fields
    }
    if (
        values["mcp"] != 0
        or values["http"] != 0
        or values["checks"] != 0
        or values["mcp_rate"] != 0
        or values["http_rate"] != 0
        or values["check_pass_rate"] != 1
    ):
        raise ValueError(f"{path} failed the correctness gate")

    check_breakdown = summary.get("check_breakdown")
    if not isinstance(check_breakdown, dict):
        raise ValueError(f"{path} has no per-operation correctness breakdown")
    for check_group in (OPERATION_SHAPE_CHECKS, OPERATION_CONTRACT_CHECKS):
        validated_operations = 0
        for check_name in check_group:
            check_counts = check_breakdown.get(check_name)
            if not isinstance(check_counts, dict):
                raise ValueError(f"{path} is missing correctness check {check_name!r}")
            passes = exact_nonnegative_integer(
                check_counts.get("passes"), f"{path} {check_name!r} passes"
            )
            fails = exact_nonnegative_integer(
                check_counts.get("fails"), f"{path} {check_name!r} failures"
            )
            if fails != 0:
                raise ValueError(f"{path} failed correctness check {check_name!r}")
            validated_operations += passes
        if operation_count > validated_operations:
            raise ValueError(
                f"{path} counted {operation_count} operations but only "
                f"{validated_operations} reached the required checks"
            )


def load_json(path: Path) -> Any:
    with path.open(encoding="utf-8") as stream:
        return json.load(stream)


def write_json(path: Path, value: Any) -> None:
    with path.open("w", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write("\n")


def interface_set(value: Any, description: str) -> set[str]:
    if (
        not isinstance(value, list)
        or any(not isinstance(item, str) or not item for item in value)
        or len(value) != len(set(value))
    ):
        raise ValueError(f"{description} is not a unique string list")
    return set(value)


def host_network_observation(
    observed_bytes: dict[str, int], audit: dict[str, Any]
) -> dict[str, Any]:
    host = audit.get("host")
    if not isinstance(host, dict):
        raise ValueError("host collector audit has no host metadata")
    if host.get("network_interface_policy") != HOST_NETWORK_POLICY:
        raise ValueError("host collector audit used an unknown network policy")

    initial = interface_set(host.get("network_interfaces"), "initial interfaces")
    active = interface_set(
        host.get("active_network_interfaces"), "active interfaces"
    )
    retired = interface_set(
        host.get("retired_network_interfaces"), "retired interfaces"
    )
    ignored_new = interface_set(
        host.get("ignored_new_network_interfaces"), "ignored new interfaces"
    )
    if not initial or active & retired or active | retired != initial:
        raise ValueError("host collector audit has an inconsistent baseline cohort")
    if ignored_new & initial:
        raise ValueError("host collector audit classifies baseline interfaces as new")

    return {
        "policy": HOST_NETWORK_POLICY,
        "scope": "baseline_interface_cohort",
        "coverage": "partial" if retired or ignored_new else "complete",
        "observed_bytes_during_collection": observed_bytes,
        "initial_interfaces": sorted(initial),
        "active_interfaces_at_end": sorted(active),
        "retired_interfaces": sorted(retired),
        "ignored_new_interfaces": sorted(ignored_new),
    }


def resource_summary(
    samples: list[dict[str, Any]],
    selected_run: int,
    *,
    host_audit: dict[str, Any] | None = None,
) -> dict[str, Any]:
    if not samples:
        raise ValueError("selected resource sample file is empty")

    cpu_values: list[float] = []
    memory_values: list[int] = []
    memory_limits: list[int] = []
    network_values: list[tuple[int, int]] = []
    for index, sample in enumerate(samples):
        if not isinstance(sample, dict):
            raise ValueError(f"resource sample {index} is not an object")
        required = {
            "cpu_percent",
            "mem_usage_bytes",
            "mem_limit_bytes",
            "net_io_rx",
            "net_io_tx",
        }
        if not required.issubset(sample):
            raise ValueError(f"resource sample {index} is incomplete")
        cpu = finite_number(sample["cpu_percent"], f"sample {index} CPU")
        memory = exact_nonnegative_integer(
            sample["mem_usage_bytes"], f"sample {index} memory"
        )
        memory_limit = exact_nonnegative_integer(
            sample["mem_limit_bytes"], f"sample {index} memory limit"
        )
        net_rx = exact_nonnegative_integer(
            sample["net_io_rx"], f"sample {index} received bytes"
        )
        net_tx = exact_nonnegative_integer(
            sample["net_io_tx"], f"sample {index} transmitted bytes"
        )
        if cpu < 0 or memory_limit <= 0:
            raise ValueError(f"resource sample {index} contains an invalid counter or limit")
        cpu_values.append(cpu)
        memory_values.append(memory)
        memory_limits.append(memory_limit)
        network_values.append((net_rx, net_tx))
    if any(
        later_rx < earlier_rx or later_tx < earlier_tx
        for (earlier_rx, earlier_tx), (later_rx, later_tx) in zip(
            network_values, network_values[1:]
        )
    ):
        raise ValueError("resource network counters are not monotonic")
    first = samples[0]
    last = samples[-1]
    observed_network_bytes = {
        "rx": network_values[-1][0] - network_values[0][0],
        "tx": network_values[-1][1] - network_values[0][1],
    }
    summary = {
        "selected_run": selected_run,
        "sample_count": len(samples),
        "first_timestamp": first.get("timestamp"),
        "last_timestamp": last.get("timestamp"),
        "cpu_percent": {
            "mean": statistics.fmean(cpu_values),
            "median": statistics.median(cpu_values),
            "max": max(cpu_values),
        },
        "memory_bytes": {
            "mean": statistics.fmean(memory_values),
            "median": statistics.median(memory_values),
            "max": max(memory_values),
            "limit": max(memory_limits),
        },
    }
    if host_audit is None:
        summary["network_bytes_during_collection"] = observed_network_bytes
    else:
        summary["network_observation"] = host_network_observation(
            observed_network_bytes, host_audit
        )
    return summary


def require_file(path: Path, description: str) -> None:
    if not path.is_file():
        raise ValueError(f"missing {description}: {path}")


def require_valid_report(path: Path, description: str) -> dict[str, Any]:
    require_file(path, description)
    report = load_json(path)
    if not isinstance(report, dict) or report.get("valid") is not True:
        raise ValueError(f"{description} is not valid: {path}")
    return report


def main() -> int:
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <results_dir> <num_runs>", file=sys.stderr)
        return 1

    results_dir = Path(sys.argv[1])
    try:
        num_runs = int(sys.argv[2])
    except ValueError:
        print("num_runs must be a positive odd number", file=sys.stderr)
        return 1
    if num_runs < 1 or num_runs % 2 == 0:
        print("num_runs must be a positive odd number", file=sys.stderr)
        return 1

    runs: list[dict[str, Any]] = []
    negotiated_versions: set[str] = set()
    for run_number in range(1, num_runs + 1):
        path = results_dir / f"k6_summary_run{run_number}.json"
        summary = load_json(path)
        validate_measurement_summary(summary, path)
        preflight_path = results_dir / f"protocol_preflight_run{run_number}.json"
        postflight_path = results_dir / f"protocol_postflight_run{run_number}.json"
        preflight = load_json(preflight_path)
        postflight = load_json(postflight_path)
        if not isinstance(preflight, dict) or not isinstance(postflight, dict):
            raise ValueError(f"run {run_number} protocol evidence is not an object")
        negotiated_version = preflight.get("negotiated_protocol_version")
        if (
            not isinstance(negotiated_version, str)
            or not negotiated_version
            or preflight.get("eligibility_contract") != ELIGIBILITY_CONTRACT
            or postflight.get("eligibility_contract") != ELIGIBILITY_CONTRACT
            or preflight.get("eligibility_valid") is not True
            or postflight.get("eligibility_valid") is not True
            or postflight.get("negotiated_protocol_version") != negotiated_version
            or summary.get("config", {}).get(
                "expected_negotiated_protocol_version"
            )
            != negotiated_version
            or summary.get("config", {}).get("eligibility_contract")
            != ELIGIBILITY_CONTRACT
        ):
            raise ValueError(f"run {run_number} has inconsistent protocol evidence")
        for phase, evidence in (("preflight", preflight), ("postflight", postflight)):
            supplemental = evidence.get("supplemental_validation")
            if not isinstance(supplemental, dict):
                raise ValueError(f"run {run_number} {phase} has no supplemental evidence")
            if supplemental.get("required") is True and supplemental.get("valid") is not True:
                raise ValueError(
                    f"run {run_number} {phase} failed required supplemental validation"
                )
        negotiated_versions.add(negotiated_version)
        require_valid_report(
            results_dir / f"resource_headroom_run{run_number}.json",
            f"run {run_number} resource report",
        )
        for cgroup_path, description in (
            (results_dir / f"cgroup_run{run_number}.json", "server cgroup evidence"),
            (results_dir / "k6" / f"cgroup_run{run_number}.json", "k6 cgroup evidence"),
        ):
            require_file(cgroup_path, description)
            cgroup = load_json(cgroup_path)
            if (
                not isinstance(cgroup, dict)
                or cgroup.get("validation", {}).get("ok") is not True
            ):
                raise ValueError(f"invalid {description}: {cgroup_path}")
        runs.append(
            {
                "run": run_number,
                "rps": operation_rps(summary),
                "summary_path": path,
                "protocol_version": negotiated_version,
            }
        )
    if len(negotiated_versions) != 1:
        raise ValueError(
            f"protocol negotiation changed between runs: {sorted(negotiated_versions)}"
        )

    ranked_runs = sorted(runs, key=lambda run: run["rps"])
    selected = ranked_runs[len(ranked_runs) // 2]
    rps_values = [float(run["rps"]) for run in runs]
    mean_rps = statistics.fmean(rps_values)
    population_cv = statistics.pstdev(rps_values) / mean_rps * 100 if mean_rps else 0.0
    sample_cv = (
        statistics.stdev(rps_values) / mean_rps * 100
        if mean_rps and len(rps_values) > 1
        else 0.0
    )
    sample_standard_deviation = statistics.stdev(rps_values) if len(rps_values) > 1 else 0.0
    # Three runs are the production default. This exact t critical value is for df=2;
    # omit the interval for other run counts rather than imply false precision.
    confidence_interval: dict[str, float] | None = None
    if len(rps_values) == 3:
        margin = 4.302652729911275 * sample_standard_deviation / math.sqrt(3)
        confidence_interval = {"low": mean_rps - margin, "high": mean_rps + margin}

    selected_run = int(selected["run"])
    shutil.copy2(selected["summary_path"], results_dir / "k6_summary.json")

    resource_files = {
        "server": ("stats", "stats.json"),
        "redis": ("redis_stats", "redis_stats.json"),
        "api_service": ("api_stats", "api_stats.json"),
        "load_generator": ("k6_stats", "k6_stats.json"),
        "host": ("host_stats", "host_stats.json"),
    }
    resources: dict[str, Any] = {"selected_run": selected_run}
    for resource_name, (run_prefix, canonical_name) in resource_files.items():
        selected_stats = results_dir / f"{run_prefix}_run{selected_run}.json"
        require_file(selected_stats, f"selected {resource_name} samples")
        shutil.copy2(selected_stats, results_dir / canonical_name)
        audit_source = results_dir / f"{run_prefix}_run{selected_run}.audit.json"
        audit_destination = results_dir / canonical_name.replace(".json", ".audit.json")
        require_file(audit_source, f"selected {resource_name} collector audit")
        shutil.copy2(audit_source, audit_destination)
        audit = load_json(audit_source)
        if not isinstance(audit, dict):
            raise ValueError(f"selected {resource_name} collector audit is not an object")
        samples = load_json(selected_stats)
        if not isinstance(samples, list):
            raise ValueError(f"selected {resource_name} samples are not a list")
        resources[resource_name] = resource_summary(
            samples,
            selected_run,
            host_audit=audit if resource_name == "host" else None,
        )
    write_json(results_dir / "resource_summary.json", resources)

    selected_files = {
        f"container_inspect_run{selected_run}.json": "container_inspect.json",
        f"image_inspect_run{selected_run}.json": "image_inspect.json",
        f"cgroup_run{selected_run}.json": "cgroup.json",
        f"resource_headroom_run{selected_run}.json": "resource_headroom.json",
        f"protocol_preflight_run{selected_run}.json": "protocol_preflight.json",
        f"protocol_postflight_run{selected_run}.json": "protocol_postflight.json",
    }
    for source_name, destination_name in selected_files.items():
        source = results_dir / source_name
        require_file(source, f"selected-run evidence {source_name}")
        shutil.copy2(source, results_dir / destination_name)

    selected_k6_dir = results_dir / "k6"
    selected_k6_dir.mkdir(exist_ok=True)
    for prefix in ("container_inspect", "image_inspect", "cgroup"):
        source = selected_k6_dir / f"{prefix}_run{selected_run}.json"
        require_file(source, f"selected k6 {prefix}")
        shutil.copy2(source, selected_k6_dir / f"{prefix}.json")

    statistics_output = {
        "runs": [{"run": run["run"], "rps": run["rps"]} for run in runs],
        "median_run": selected_run,
        "median_rps": selected["rps"],
        "mean_rps": mean_rps,
        "population_cv_pct": population_cv,
        "sample_cv_pct": sample_cv,
        "cv_pct": sample_cv,
        "mean_rps_95pct_confidence_interval": confidence_interval,
        "negotiated_protocol_version": next(iter(negotiated_versions)),
        "eligibility_contract": ELIGIBILITY_CONTRACT,
    }
    write_json(results_dir / "k6_multi_run_stats.json", statistics_output)
    print(
        f"Median run: {selected_run} (RPS={selected['rps']:.2f}), "
        f"sample CV={sample_cv:.2f}%"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
