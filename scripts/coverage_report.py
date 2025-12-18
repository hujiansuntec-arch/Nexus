import os
import glob
import subprocess
import sys

def generate_report():
    build_dir = "build_test"
    source_root = os.getcwd()
    
    # Find all gcda files
    gcda_files = glob.glob(os.path.join(build_dir, "**/*.gcda"), recursive=True)
    
    total_lines = 0
    total_executed = 0
    
    file_stats = []

    print(f"{'File':<60} | {'Coverage':<10} | {'Lines':<10}")
    print("-" * 86)

    for gcda in gcda_files:
        # Construct path to gcno
        gcno = gcda.replace(".gcda", ".gcno")
        if not os.path.exists(gcno):
            continue
            
        # Make gcno absolute because we might change cwd or just to be safe
        gcno_abs = os.path.abspath(gcno)
            
        parts = gcda.split(os.sep)
        try:
            src_idx = parts.index("src")
            source_rel = os.path.join(*parts[src_idx:]).replace(".gcda", "")
        except ValueError:
            continue
            
        source_abs = os.path.join(source_root, source_rel)
        if not os.path.exists(source_abs):
            continue
            
        # Run gcov
        # We run from build_dir so .gcov files are generated there
        cmd = ["gcov", "-o", gcno_abs, source_abs]
        
        try:
            result = subprocess.run(cmd, cwd=build_dir, capture_output=True, text=True)
            output = result.stdout
            
            lines = output.splitlines()
            for i, line in enumerate(lines):
                if line.startswith("File '"):
                    gcov_file = line[6:-1]
                    if os.path.abspath(gcov_file) == source_abs:
                        if i + 1 < len(lines):
                            next_line = lines[i+1]
                            if "Lines executed:" in next_line:
                                parts = next_line.split(":")
                                if len(parts) > 1:
                                    data = parts[1].split()
                                    percent_str = data[0].replace("%", "")
                                    count_str = data[2]
                                    
                                    percent = float(percent_str)
                                    count = int(count_str)
                                    executed = int(count * percent / 100.0)
                                    
                                    total_lines += count
                                    total_executed += executed
                                    
                                    file_stats.append((source_rel, percent, count))
                                    print(f"{source_rel:<60} | {percent:>9.2f}% | {count:>10}")
        except Exception as e:
            print(f"Error processing {source_rel}: {e}")

    print("-" * 86)
    if total_lines > 0:
        total_percent = (total_executed / total_lines) * 100
        print(f"{'TOTAL':<60} | {total_percent:>9.2f}% | {total_lines:>10}")
    else:
        print("No coverage data found.")

    # Cleanup .gcov files
    gcov_files = glob.glob(os.path.join(build_dir, "*.gcov"))
    for f in gcov_files:
        os.remove(f)

if __name__ == "__main__":
    generate_report()
