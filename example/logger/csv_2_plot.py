import argparse
import matplotlib.dates as mdates
import matplotlib.pyplot as plt
import pandas as pd
from dateutil.tz import gettz


def plot_power_data(csv_path, output_path, plot_types, sources):
    """
    Reads power data from a CSV file and generates a plot image.

    Args:
        csv_path (str): The path to the input CSV file.
        output_path (str): The path to save the output plot image.
        plot_types (list): A list of strings indicating which plots to generate
                           (e.g., ['power', 'voltage', 'current']).
        sources (list): A list of strings indicating which power sources to plot
                        (e.g., ['vin', 'main', 'usb']).
    """
    try:
        # Read the CSV file into a pandas DataFrame
        # The 'timestamp' column is parsed as dates. Pandas automatically recognizes
        # the ISO format (with 'Z') as UTC.
        df = pd.read_csv(csv_path, parse_dates=['timestamp'])
        print(f"Successfully loaded {len(df)} records from '{csv_path}'")

        # --- Timezone Conversion ---
        # Get the system's local timezone
        local_tz = gettz()
        # The timestamp from CSV is already UTC-aware.
        # Convert it to the system's local timezone for plotting.
        df['timestamp'] = df['timestamp'].dt.tz_convert(local_tz)
        print(f"Timestamp converted to local timezone: {local_tz}")

    except FileNotFoundError:
        print(f"Error: The file '{csv_path}' was not found.")
        return
    except Exception as e:
        print(f"An error occurred while reading the CSV file: {e}")
        return

    # --- Plotting Configuration ---
    # Y-axis scale settings from chart.js
    scale_config = {
        'power': {'steps': [5, 20, 50, 160]},
        'voltage': {'steps': [5, 10, 15, 25]},
        'current': {'steps': [1, 2.5, 5, 10]}
    }

    plot_configs = {
        'power': {'title': 'Power Consumption', 'ylabel': 'Power (W)',
                  'cols': [f'{s}_power' for s in sources]},
        'voltage': {'title': 'Voltage', 'ylabel': 'Voltage (V)',
                    'cols': [f'{s}_voltage' for s in sources]},
        'current': {'title': 'Current', 'ylabel': 'Current (A)',
                    'cols': [f'{s}_current' for s in sources]}
    }
    channel_labels = [s.upper() for s in sources]
    # Define a color map for all possible sources
    color_map = {'vin': 'red', 'main': 'green', 'usb': 'blue'}
    channel_colors = [color_map[s] for s in sources]

    num_plots = len(plot_types)
    if num_plots == 0:
        print("No plot types selected. Exiting.")
        return

    # Create a figure and a set of subplots based on the number of selected plot types.
    fig, axes = plt.subplots(num_plots, 1, figsize=(15, 6 * num_plots), sharex=True, squeeze=False)
    axes = axes.flatten()  # Flatten the 2D array to 1D for easier iteration

    # --- Loop through selected plot types and generate plots ---
    for i, plot_type in enumerate(plot_types):
        ax = axes[i]
        config = plot_configs[plot_type]

        max_data_value = 0
        for j, col_name in enumerate(config['cols']):
            if col_name in df.columns:
                ax.plot(df['timestamp'], df[col_name], label=channel_labels[j], color=channel_colors[j])
                # Find the maximum value in the current column to set the y-axis limit
                max_col_value = df[col_name].max()
                if max_col_value > max_data_value:
                    max_data_value = max_col_value
            else:
                print(f"Warning: Column '{col_name}' not found in CSV. Skipping.")

        # --- Dynamic Y-axis Scaling ---
        ax.set_ylim(bottom=0) # Set y-axis minimum to 0
        if plot_type in scale_config:
            steps = scale_config[plot_type]['steps']
            # Find the smallest step that is >= max_data_value
            new_max = next((step for step in steps if step >= max_data_value), steps[-1])
            ax.set_ylim(top=new_max)

        ax.set_title(config['title'])
        ax.set_ylabel(config['ylabel'])
        ax.legend()
        ax.grid(True, which='both', linestyle='--', linewidth=0.5)

    # --- Formatting the x-axis (Time) ---
    local_tz = gettz()
    last_ax = axes[-1]
    # Pass the timezone to the formatter
    last_ax.xaxis.set_major_formatter(mdates.DateFormatter('%H:%M:%S', tz=local_tz))
    last_ax.xaxis.set_major_locator(plt.MaxNLocator(15))  # Limit the number of ticks
    plt.xlabel(f'Time ({local_tz.tzname(df["timestamp"].iloc[-1])})') # Display timezone name
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
    parser.add_argument(
        "-s", "--source",
        nargs='+',
        choices=['vin', 'main', 'usb'],
        default=['vin', 'main', 'usb'],
        help="Power sources to plot. Choose from 'vin', 'main', 'usb'. "
             "Default is to plot all three."
    )
    args = parser.parse_args()

    plot_power_data(args.input_csv, args.output_image, args.type, args.source)


if __name__ == "__main__":
    main()
