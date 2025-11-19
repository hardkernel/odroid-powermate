import argparse
import matplotlib.dates as mdates
import matplotlib.pyplot as plt
import os
import pandas as pd


def plot_power_data(csv_path, output_path):
    """
    Reads power data from a CSV file and generates a plot image.

    Args:
        csv_path (str): The path to the input CSV file.
        output_path (str): The path to save the output plot image.
    """
    try:
        # Read the CSV file into a pandas DataFrame
        # The 'timestamp' column is parsed as dates
        df = pd.read_csv(csv_path, parse_dates=['timestamp'])
        print(f"Successfully loaded {len(df)} records from '{csv_path}'")
    except FileNotFoundError:
        print(f"Error: The file '{csv_path}' was not found.")
        return
    except Exception as e:
        print(f"An error occurred while reading the CSV file: {e}")
        return

    # Create a figure and a set of subplots (3 rows, 1 column)
    # sharex=True makes all subplots share the same x-axis (time)
    fig, axes = plt.subplots(3, 1, figsize=(15, 18), sharex=True)

    # --- Plot 1: Power (W) ---
    ax1 = axes[0]
    ax1.plot(df['timestamp'], df['vin_power'], label='VIN', color='red')
    ax1.plot(df['timestamp'], df['main_power'], label='MAIN', color='green')
    ax1.plot(df['timestamp'], df['usb_power'], label='USB', color='blue')
    ax1.set_title('Power Consumption')
    ax1.set_ylabel('Power (W)')
    ax1.legend()
    ax1.grid(True, which='both', linestyle='--', linewidth=0.5)

    # --- Plot 2: Voltage (V) ---
    ax2 = axes[1]
    ax2.plot(df['timestamp'], df['vin_voltage'], label='VIN', color='red')
    ax2.plot(df['timestamp'], df['main_voltage'], label='MAIN', color='green')
    ax2.plot(df['timestamp'], df['usb_voltage'], label='USB', color='blue')
    ax2.set_title('Voltage')
    ax2.set_ylabel('Voltage (V)')
    ax2.legend()
    ax2.grid(True, which='both', linestyle='--', linewidth=0.5)

    # --- Plot 3: Current (A) ---
    ax3 = axes[2]
    ax3.plot(df['timestamp'], df['vin_current'], label='VIN', color='red')
    ax3.plot(df['timestamp'], df['main_current'], label='MAIN', color='green')
    ax3.plot(df['timestamp'], df['usb_current'], label='USB', color='blue')
    ax3.set_title('Current')
    ax3.set_ylabel('Current (A)')
    ax3.legend()
    ax3.grid(True, which='both', linestyle='--', linewidth=0.5)

    # --- Formatting the x-axis (Time) ---
    # Improve date formatting on the x-axis
    ax3.xaxis.set_major_formatter(mdates.DateFormatter('%H:%M:%S'))
    ax3.xaxis.set_major_locator(plt.MaxNLocator(15))  # Limit the number of ticks
    plt.xlabel('Time')
    plt.xticks(rotation=45)

    # Add a main title to the figure
    start_time = df['timestamp'].iloc[0].strftime('%Y-%m-%d %H:%M:%S')
    end_time = df['timestamp'].iloc[-1].strftime('%H:%M:%S')
    fig.suptitle(f'ODROID Power Log ({start_time} to {end_time})', fontsize=16, y=0.95)

    # Adjust layout to prevent titles/labels from overlapping
    plt.tight_layout(rect=[0, 0, 1, 0.94])

    # --- Save the plot to a file ---
    try:
        plt.savefig(output_path, dpi=150)
        print(f"Plot successfully saved to '{output_path}'")
    except Exception as e:
        print(f"An error occurred while saving the plot: {e}")


def main():
    parser = argparse.ArgumentParser(description="Generate a plot from an Odroid PowerMate CSV log file.")
    parser.add_argument("input_csv", help="Path to the input CSV log file.")
    parser.add_argument("output_image", help="Path to save the output plot image (e.g., plot.png).")
    args = parser.parse_args()

    plot_power_data(args.input_csv, args.output_image)


if __name__ == "__main__":
    main()
