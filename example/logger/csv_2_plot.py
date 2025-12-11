import argparse
import matplotlib
matplotlib.use('Agg')
import matplotlib.dates as mdates
import matplotlib.pyplot as plt
import pandas as pd
from dateutil.tz import gettz
from matplotlib.ticker import MultipleLocator, FuncFormatter
import math
from scipy.signal import savgol_filter
from scipy.ndimage import gaussian_filter1d


def plot_power_data(csv_path, output_path, plot_types, sources,
                    voltage_y_max=None, current_y_max=None, power_y_max=None,
                    relative_time=False, time_x_line=None, time_x_label=None,
                    filter_type=None, window_size=None):
    """
    Reads power data from a CSV file and generates a plot image.

    Args:
        csv_path (str): The path to the input CSV file.
        output_path (str): The path to save the output plot image.
        plot_types (list): A list of strings indicating which plots to generate.
        sources (list): A list of strings indicating which power sources to plot.
        voltage_y_max (float, optional): Maximum value for the voltage plot's Y-axis.
        current_y_max (float, optional): Maximum value for the current plot's Y-axis.
        power_y_max (float, optional): Maximum value for the power plot's Y-axis.
        relative_time (bool): If True, the x-axis will show elapsed time from the start.
        time_x_line (float, optional): Interval in seconds for x-axis grid lines.
        time_x_label (float, optional): Interval in seconds for x-axis labels.
        filter_type (str, optional): The type of filter to apply.
        window_size (int, optional): The window size for the filter in seconds.
    """
    try:
        # Read the CSV file into a pandas DataFrame
        df = pd.read_csv(csv_path, parse_dates=['timestamp'])
        print(f"Successfully loaded {len(df)} records from '{csv_path}'")

        if df.empty:
            print("CSV file is empty. Exiting.")
            return

        # --- Time Handling ---
        x_axis_data = df['timestamp']
        if relative_time:
            start_time = df['timestamp'].iloc[0]
            df.loc[:, 'elapsed_seconds'] = (df['timestamp'] - start_time).dt.total_seconds()
            x_axis_data = df['elapsed_seconds']
            print("X-axis set to relative time (elapsed seconds).")
        else:
            # --- Timezone Conversion for absolute time ---
            local_tz = gettz()
            df.loc[:, 'timestamp'] = df['timestamp'].dt.tz_convert(local_tz)
            print(f"Timestamp converted to local timezone: {local_tz}")

    except FileNotFoundError:
        print(f"Error: The file '{csv_path}' was not found.")
        return
    except Exception as e:
        print(f"An error occurred while reading the CSV file: {e}")
        return

    # --- Calculate Average Interval ---
    avg_interval_s = 0
    if len(df) > 1:
        avg_interval = df['timestamp'].diff().mean()
        avg_interval_s = avg_interval.total_seconds()
        avg_interval_ms = avg_interval_s * 1000

    # --- Auto-set window size if not provided ---
    if filter_type and window_size is None and avg_interval_s > 0:
        window_size = avg_interval_s * 15  # Default to 15x the average interval
        print(f"Window size not specified. Automatically set to {window_size:.2f} seconds.")


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
                # Plot original data
                ax.plot(x_axis_data, df[col_name], label=f'{channel_labels[j]} (Raw)', color=channel_colors[j], alpha=0.5, zorder=2)
                max_col_value = df[col_name].max()
                if max_col_value > max_data_value:
                    max_data_value = max_col_value

                # --- Apply and plot filtered data ---
                if filter_type and window_size:
                    # Calculate window size in samples
                    if avg_interval_s > 0:
                        window_samples = int(window_size / avg_interval_s)
                        if window_samples < 1:
                            window_samples = 1
                        
                        filtered_data = None
                        filter_label = ""

                        if filter_type == 'savgol':
                            # Window size for savgol must be odd
                            if window_samples % 2 == 0:
                                window_samples += 1
                            if window_samples > 2: # Polyorder must be less than window_length
                                filtered_data = savgol_filter(df[col_name], window_samples, 2) # polynomial order 2
                                filter_label = "Savitzky-Golay"
                        elif filter_type == 'moving_average':
                            filtered_data = df[col_name].rolling(window=window_samples, center=True).mean()
                            filter_label = "Moving Average"
                        elif filter_type == 'ema':
                            filtered_data = df[col_name].ewm(span=window_samples, adjust=False).mean()
                            filter_label = "Exponential Moving Average"
                        elif filter_type == 'gaussian':
                            # Sigma is a fraction of the window size
                            sigma = window_samples / 4.0
                            filtered_data = gaussian_filter1d(df[col_name], sigma=sigma)
                            filter_label = "Gaussian"


                        if filtered_data is not None:
                             ax.plot(x_axis_data, filtered_data, label=f'{channel_labels[j]} ({filter_label})', color=channel_colors[j], linestyle='-', linewidth=1.5, zorder=3)

            else:
                print(f"Warning: Column '{col_name}' not found in CSV. Skipping.")

        # --- Dynamic Y-axis Scaling ---
        ax.set_ylim(bottom=0)
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

        # --- Y-Grid and Tick Configuration ---
        y_min, y_max = ax.get_ylim()
        
        if y_max <= 0:
            major_interval = 1.0 # Default for very small or zero range
        elif plot_type == 'current' and y_max <= 2.5:
            major_interval = 0.5 # Maintain current behavior for very small current values
        elif y_max <= 10:
            major_interval = 2.0 # Maintain current behavior for small ranges where 5-unit is too coarse
        elif y_max <= 25:
            major_interval = 5.0 # Already a multiple of 5
        else: # y_max > 25
            # Aim for major ticks that are multiples of 5.
            # Calculate a rough interval to get around 5 major ticks.
            rough_interval = y_max / 5.0
            
            # Find the smallest multiple of 5 that is greater than or equal to rough_interval.
            # This ensures labels are multiples of 5.
            major_interval = math.ceil(rough_interval / 5.0) * 5.0
            
            # Ensure major_interval is not 0 if y_max is small but positive.
            if major_interval == 0 and y_max > 0:
                major_interval = 5.0

        ax.yaxis.set_major_locator(MultipleLocator(major_interval))
        ax.yaxis.set_minor_locator(MultipleLocator(1))
        ax.yaxis.grid(False, which='major')
        ax.yaxis.grid(True, which='minor', linestyle='--', linewidth=0.6, zorder=0)

        for y_val in range(int(y_min), int(y_max) + 1):
            if y_val == 0: continue
            if y_val % 10 == 0:
                ax.axhline(y=y_val, color='maroon', linestyle='--', linewidth=1.2, zorder=1)
            elif y_val % 5 == 0:
                ax.axhline(y=y_val, color='midnightblue', linestyle='--', linewidth=1.2, zorder=1)

        # --- X-Grid Configuration ---
        ax.xaxis.grid(True, which='major', linestyle='--', linewidth=0.8)
        if time_x_line is not None:
            ax.xaxis.grid(True, which='minor', linestyle=':', linewidth=0.6)


    # --- Formatting the x-axis ---
    last_ax = axes[-1]
    if not df.empty:
        last_ax.set_xlim(x_axis_data.iloc[0], x_axis_data.iloc[-1])

    if relative_time:
        plt.xlabel('Elapsed Time (seconds)')
        if time_x_label is not None:
            last_ax.xaxis.set_major_locator(MultipleLocator(time_x_label))
        else:
            last_ax.xaxis.set_major_locator(plt.MaxNLocator(15))
        if time_x_line is not None:
            last_ax.xaxis.set_minor_locator(MultipleLocator(time_x_line))
    else:
        local_tz = gettz()
        plt.xlabel(f'Time ({local_tz.tzname(df["timestamp"].iloc[-1])})')
        last_ax.xaxis.set_major_formatter(mdates.DateFormatter('%H:%M:%S', tz=local_tz))
        if time_x_label is not None:
            last_ax.xaxis.set_major_locator(mdates.SecondLocator(interval=int(time_x_label)))
        else:
            last_ax.xaxis.set_major_locator(plt.MaxNLocator(15))
        if time_x_line is not None:
            last_ax.xaxis.set_minor_locator(mdates.SecondLocator(interval=int(time_x_line)))

    plt.xticks(rotation=45)

    # --- Add a main title and subtitle ---
    if relative_time:
        main_title = 'PowerMate Log'
    else:
        start_time_str = df['timestamp'].iloc[0].strftime('%Y-%m-%d %H:%M:%S')
        end_time_str = df['timestamp'].iloc[-1].strftime('%H:%M:%S')
        main_title = f'PowerMate Log ({start_time_str} to {end_time_str})'


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
    parser.add_argument(
        "-r", "--relative-time",
        action='store_true',
        help="Display the x-axis as elapsed time from the start (in seconds) instead of absolute time."
    )
    parser.add_argument("--time_x_line", type=float, help="Interval in seconds for x-axis grid lines.")
    parser.add_argument("--time_x_label", type=float, help="Interval in seconds for x-axis labels.")
    parser.add_argument(
        '--filter',
        choices=['savgol', 'moving_average', 'ema', 'gaussian'],
        help='The type of filter to apply to the data.'
    )
    parser.add_argument(
        '--window_size',
        type=float,
        help='The window size for the filter in seconds.'
    )


    args = parser.parse_args()

    plot_power_data(
        args.input_csv,
        args.output_image,
        args.type,
        args.source,
        voltage_y_max=args.voltage_y_max,
        current_y_max=args.current_y_max,
        power_y_max=args.power_y_max,
        relative_time=args.relative_time,
        time_x_line=args.time_x_line,
        time_x_label=args.time_x_label,
        filter_type=args.filter,
        window_size=args.window_size
    )


if __name__ == "__main__":
    main()
