import os
import subprocess
import re
import argparse

DUCKDB_BINARY = "build/release/duckdb"

def run_benchmark(benchmark_path):
    # Spawn a DuckDB subprocess to run the benchmark
    try:
        result = subprocess.run(
            [DUCKDB_BINARY, "-f", benchmark_path],
            capture_output=True,
            text=True,
            check=True
        )
        # Parse the timing output (Run Time (s): real 0.702 user 5.232869 sys 0.014099, using a regex
        output = result.stdout
        timing_match = re.search(r"Run Time \(s\): real ([\d.]+) user [\d.]+ sys [\d.]+", output)
        if timing_match:
            run_time = float(timing_match.group(1))
            return run_time
        else:
            print("Could not find run time in the output.")
            exit(1)

    except subprocess.CalledProcessError as e:
        print("Error running benchmark:")
        print(e.stderr)
        print("Return code:", e.returncode)

BENCHMARKS = {
    "area": [
        "benchmark/bkb_area.test",
        "benchmark/bkb_area_fast.test",
        "benchmark/wkb_area.test",
    ],
    "extent": [
        "benchmark/bkb_extent.test",
        "benchmark/bkb_extent_fast.test",
        "benchmark/wkb_extent.test",
    ],
    "to_wkb": [
        "benchmark/bkb_to_wkb.test",
        "benchmark/wkb_to_wkb_cast.test",
        "benchmark/wkb_to_wkb_copy.test",
    ],
    "from_wkb": [
        "benchmark/bkb_from_wkb.test",
        "benchmark/wkb_from_wkb.test",
        "benchmark/wkb_from_wkb_le.test",
    ],
}


if __name__ == "__main__":
    # Parse command line arguments
    # -b, --benchmark: specify a benchmark to run
    parser = argparse.ArgumentParser(description="Run DuckDB benchmarks.")
    parser.add_argument(
        "-b", "--benchmark",
        choices=list(BENCHMARKS.keys()),
        help="Specify a benchmark to run. If not provided, all benchmarks will be run."
    )

    parser.add_argument(
        "-n", "--num-runs",
        type=int,
        default=3,
        help="Number of times to run each benchmark (default: 3)."
    )

    parser.add_argument(
        "-d", "--dataset",
        type=str,
        default="buildings.parquet",
        help="Path to the dataset file to use in benchmarks (default: 'buildings.parquet')."
    )

    args = parser.parse_args()
    # If a specific benchmark is provided, filter the benchmarks
    if args.benchmark:
        BENCHMARKS = {args.benchmark: BENCHMARKS[args.benchmark]}

    for benchmark_name, benchmark_paths in BENCHMARKS.items():
        print(f"Running benchmarks for '{benchmark_name}':")
        for benchmark_path in benchmark_paths:
            # Make a temp file to store the benchmark file
            tmp_name = os.path.basename(benchmark_path)
            tmp_dir = os.path.dirname(benchmark_path)
            tmp_file = os.path.join(tmp_dir, 'tmp', f"tmp_{tmp_name}")
            os.makedirs(os.path.dirname(tmp_file), exist_ok=True)

            # Copy the benchmark file to the temp file
            with open(benchmark_path, 'r') as src_file:
                bench_text = src_file.read().replace('${DATASET}', args.dataset)
                with open(tmp_file, 'w') as dst_file:
                    dst_file.write(bench_text)


            # Run each benchmark the specified number of times
            times = []
            for _ in range(args.num_runs):
                run_time = run_benchmark(tmp_file)
                times.append(run_time)
            # Calculate the average run time
            average_time = sum(times) / len(times)
            # Calculate standard deviation
            std_dev = (sum((x - average_time) ** 2 for x in times) / len(times)) ** 0.5

            print(f" - {benchmark_path:<40}{average_time:.3f} seconds (+/- {std_dev:.3f})")