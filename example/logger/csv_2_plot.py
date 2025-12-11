import argparse
import matplotlib.dates as mdates
import matplotlib.pyplot as plt
import pandas as pd
from dateutil.tz import gettz
from matplotlib.ticker import MultipleLocator


def plot_power_data(csv_path, output_path, plot_types, sources,
                    voltage_y_max=None, current_y_max=None, power_y_max=None):
    """
    Reads power data from a CSV file and generates a plot image.

    Args:
        csv_path (str): The path to the input CSV file.
        output_path (str): The path to save the output plot image.
        plot_types (list): A list of strings indicating which plots to generate
                           (e.g., ['power', 'voltage', 'current']).
        sources (list): A list of strings indicating which power sources to plot
                        (e.g., ['vin', 'main', 'usb']).
        voltage_y_max (float, optional): Maximum value for the voltage plot's Y-axis.
        current_y_max (float, optional): Maximum value for the current plot's Y-axis.
        power_y_max (float, optional): Maximum value for the power plot's Y-axis.
    """
    try:
        # Read the CSV file into a pandas DataFrame
        df = pd.read_csv(csv_path, parse_dates=['timestamp'])
        print(f"Successfully loaded {len(df)} records from '{csv_path}'")

        # --- Timezone Conversion ---
        local_tz = gettz()
        df['timestamp'] = df['timestamp'].dt.tz_convert(local_tz)
        print(f"Timestamp converted to local timezone: {local_tz}")

    except FileNotFoundError:
        print(f"Error: The file '{csv_path}' was not found.")
        return
    except Exception as e:
        print(f"An error occurred while reading the CSV file: {e}")
        return

    # --- Calculate Average Interval ---
    avg_interval_ms = 0
    if len(df) > 1:
        avg_interval = df['timestamp'].diff().mean()
        avg_interval_ms = avg_interval.total_seconds() * 1000

    # --- Calculate Average Voltages ---
    avg_voltages = {}
    for source in sources:
        voltage_col = f'{source}_voltage'
        if voltage_col in df.columns:
            avg_voltages[source] = df[voltage_col].mean()

    # --- Plotting Configuration ---
    scale_config = {
        'power': {'steps': [5, 20, 50, 160]},
        'voltage': {'steps': [5, 10, 15, 25]},
        'current': {'steps': [1, 2.5, 5, 10]}
    }
    plot_configs = {
        'power': {'title': 'Power Consumption', 'ylabel': 'Power (W)', 'cols': [f'{s}_power' for s in sources]},
        'voltage': {'title': 'Voltage', 'ylabel': 'Voltage (V)', 'cols': [f'{s}_voltage' for s in sources]},
        'current': {'title': 'Current', 'ylabel': 'Current (A)', 'cols': [f'{s}_current' for s in sources]}
    }
    y_max_options = {
        'power': power_y_max,
        'voltage': voltage_y_max,
        'current': current_y_max
    }

    channel_labels = [s.upper() for s in sources]
    color_map = {'vin': 'red', 'main': 'green', 'usb': 'blue'}
    channel_colors = [color_map[s] for s in sources]

    num_plots = len(plot_types)
    if num_plots == 0:
        print("No plot types selected. Exiting.")
        return

    fig, axes = plt.subplots(num_plots, 1, figsize=(15, 9 * num_plots), sharex=True, squeeze=False)
    axes = axes.flatten()

    # --- Loop through selected plot types and generate plots ---
    for i, plot_type in enumerate(plot_types):
        ax = axes[i]
        config = plot_configs[plot_type]
        max_data_value = 0
        for j, col_name in enumerate(config['cols']):
            if col_name in df.columns:
                ax.plot(df['timestamp'], df[col_name], label=channel_labels[j], color=channel_colors[j], zorder=2)
                max_col_value = df[col_name].max()
                if max_col_value > max_data_value:
                    max_data_value = max_col_value
            else:
                print(f"Warning: Column '{col_name}' not found in CSV. Skipping.")

        # --- Dynamic Y-axis Scaling ---
        ax.set_ylim(bottom=0)
        # Set y-axis max from options if provided
        y_max_option = y_max_options.get(plot_type)
        if y_max_option is not None:
            ax.set_ylim(top=y_max_option)
        elif plot_type in scale_config:
            steps = scale_config[plot_type]['steps']
            new_max = next((step for step in steps if step >= max_data_value), steps[-1])
            ax.set_ylim(top=new_max)

        ax.set_title(config['title'])
        ax.set_ylabel(config['ylabel'])
        ax.legend()

        # --- Grid and Tick Configuration ---
        y_min, y_max = ax.get_ylim()

        # Keep the dynamic major_interval logic for tick LABELS
        if plot_type == 'current' and y_max <= 2.5:
            major_interval = 0.5
        elif y_max <= 10:
            major_interval = 2
        elif y_max <= 25:
            major_interval = 5
        else:
            major_interval = y_max / 5.0

        ax.yaxis.set_major_locator(MultipleLocator(major_interval))
        ax.yaxis.set_minor_locator(MultipleLocator(1))

        # Disable the default major grid, but keep the minor one
        ax.yaxis.grid(False, which='major')
        ax.yaxis.grid(True, which='minor', linestyle='--', linewidth=0.6, zorder=0)

        # Draw custom lines for 5 and 10 multiples, which are now the only major grid lines
        for y_val in range(int(y_min), int(y_max) + 1):
            if y_val == 0: continue
            if y_val % 10 == 0:
                ax.axhline(y=y_val, color='maroon', linestyle='--', linewidth=1.2, zorder=1)
            elif y_val % 5 == 0:
                ax.axhline(y=y_val, color='midnightblue', linestyle='--', linewidth=1.2, zorder=1)

        # Keep the x-axis grid
        ax.xaxis.grid(True, which='major', linestyle='--', linewidth=0.8)

    # --- Formatting the x-axis (Time) ---
    local_tz = gettz()
    last_ax = axes[-1]

    if not df.empty:
        last_ax.set_xlim(df['timestamp'].iloc[0], df['timestamp'].iloc[-1])

    last_ax.xaxis.set_major_formatter(mdates.DateFormatter('%H:%M:%S', tz=local_tz))
    last_ax.xaxis.set_major_locator(plt.MaxNLocator(15))
    plt.xlabel(f'Time ({local_tz.tzname(df["timestamp"].iloc[-1])})')
    plt.xticks(rotation=45)

    # --- Add a main title and subtitle ---
    start_time = df['timestamp'].iloc[0].strftime('%Y-%m-%d %H:%M:%S')
    end_time = df['timestamp'].iloc[-1].strftime('%H:%M:%S')
    main_title = f'PowerMate Log ({start_time} to {end_time})'

    subtitle_parts = []
    if avg_interval_ms > 0:
        subtitle_parts.append(f'Avg. Interval: {avg_interval_ms:.2f} ms')

    voltage_strings = [f'{source.upper()} Avg: {avg_v:.2f} V' for source, avg_v in avg_voltages.items()]
    if voltage_strings:
        subtitle_parts.extend(voltage_strings)

    subtitle = ' | '.join(subtitle_parts)

    full_title = main_title
    if subtitle:
        full_title += f'\n{subtitle}'

    fig.suptitle(full_title, fontsize=14)

    # Adjust layout to make space for the subtitle
    plt.tight_layout(rect=[0, 0, 1, 0.98])

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
    parser.add_argument("--voltage_y_max", type=float, help="Maximum value for the voltage plot's Y-axis.")
    parser.add_argument("--current_y_max", type=float, help="Maximum value for the current plot's Y-axis.")
    parser.add_argument("--power_y_max", type=float, help="Maximum value for the power plot's Y-axis.")

    args = parser.parse_args()

    plot_power_data(
        args.input_csv,
        args.output_image,
        args.type,
        args.source,
        voltage_y_max=args.voltage_y_max,
        current_y_max=args.current_y_max,
        power_y_max=args.power_y_max
    )


if __name__ == "__main__":
    main()
