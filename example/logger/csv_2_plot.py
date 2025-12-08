import argparse

import matplotlib.dates as mdates
import matplotlib.pyplot as plt
import pandas as pd


def plot_power_data(csv_path, output_path, plot_types):
    """
    Reads power data from a CSV file and generates a plot image.

    Args:
        csv_path (str): The path to the input CSV file.
        output_path (str): The path to save the output plot image.
        plot_types (list): A list of strings indicating which plots to generate
                           (e.g., ['power', 'voltage', 'current']).
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

    # --- Plotting Configuration ---
    plot_configs = {
        'power': {'title': 'Power Consumption', 'ylabel': 'Power (W)',
                  'cols': ['vin_power', 'main_power', 'usb_power']},
        'voltage': {'title': 'Voltage', 'ylabel': 'Voltage (V)',
                    'cols': ['vin_voltage', 'main_voltage', 'usb_voltage']},
        'current': {'title': 'Current', 'ylabel': 'Current (A)', 'cols': ['vin_current', 'main_current', 'usb_current']}
    }
    channel_labels = ['VIN', 'MAIN', 'USB']
    channel_colors = ['red', 'green', 'blue']

    num_plots = len(plot_types)
    if num_plots == 0:
        print("No plot types selected. Exiting.")
        return

    # Create a figure and a set of subplots based on the number of selected plot types.
    # sharex=True makes all subplots share the same x-axis (time)
    # squeeze=False ensures that 'axes' is always a 2D array, even if num_plots is 1.
    fig, axes = plt.subplots(num_plots, 1, figsize=(15, 6 * num_plots), sharex=True, squeeze=False)
    axes = axes.flatten()  # Flatten the 2D array to 1D for easier iteration

    # --- Loop through selected plot types and generate plots ---
    for i, plot_type in enumerate(plot_types):
        ax = axes[i]
        config = plot_configs[plot_type]

        for j, col_name in enumerate(config['cols']):
            ax.plot(df['timestamp'], df[col_name], label=channel_labels[j], color=channel_colors[j])

        ax.set_title(config['title'])
        ax.set_ylabel(config['ylabel'])
        ax.legend()
        ax.grid(True, which='both', linestyle='--', linewidth=0.5)

    # --- Formatting the x-axis (Time) ---
    # Improve date formatting on the x-axis
    # Apply formatting to the last subplot's x-axis
    last_ax = axes[-1]
    last_ax.xaxis.set_major_formatter(mdates.DateFormatter('%H:%M:%S'))
    last_ax.xaxis.set_major_locator(plt.MaxNLocator(15))  # Limit the number of ticks
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
    parser.add_argument(
        "-t", "--type",
        nargs='+',
        choices=['power', 'voltage', 'current'],
        default=['power', 'voltage', 'current'],
        help="Types of plots to generate. Choose from 'power', 'voltage', 'current'. "
             "Default is to generate all three."
    )
    args = parser.parse_args()

    plot_power_data(args.input_csv, args.output_image, args.type)


if __name__ == "__main__":
    main()
