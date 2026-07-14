import sys

def read_data(filename):
    data = []
    with open(filename, 'r') as f:
        for line in f:
            if line.strip():
                try:
                    time_str, value_str = line.strip().split()
                    time = float(time_str)
                    value = float(value_str)
                    data.append((time, value))
                except ValueError:
                    print("Wrong!!!!!!!!!!")
    return data

def moving_average(data, window_size=3):
    result = []
    for i in range(len(data)):
        start = max(0, i - window_size + 1)
        window = data[start:i + 1]
        avg = sum(value for _, value in window) / len(window)
        result.append((data[i][0], avg))
    return result

def main():
    if len(sys.argv) < 2:
        print("HowToUse: python moving_avg.py <input_file>", file=sys.stderr)
        sys.exit(1)

    input_file = sys.argv[1]
    data = read_data(input_file)
    smoothed = moving_average(data, window_size=10)

    for time, avg in smoothed:
        print(f"{time:.3f} {avg:.3f}")

if __name__ == "__main__":
    main()

