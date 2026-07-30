package main

import (
	"fmt"
	"image/color"
	"math"
	"strings"

	"charm.land/lipgloss/v2"
	"github.com/charmbracelet/x/ansi"
)

const (
	maxGraphSamples                = 600
	minMetricGraphWidth            = 8
	minMetricGraphHeight           = 6
	minHorizontalMetricGraphHeight = 7
)

type graphHistory struct {
	voltage [3][]float64
	current [3][]float64
	power   [3][]float64
}

type graphLayout uint8

const (
	graphLayoutAuto graphLayout = iota
	graphLayoutHorizontal
	graphLayoutVertical
)

type metricDefinition struct {
	title     string
	unit      string
	precision int
	values    *[3][]float64
	current   [3]float64
}

func (history *graphHistory) Add(sensor sensorData) {
	channels := [3]channelData{sensor.VIN, sensor.Main, sensor.USB}
	for index, channel := range channels {
		history.add(&history.voltage[index], float64(channel.Voltage))
		history.add(&history.current[index], float64(channel.Current))
		history.add(&history.power[index], float64(channel.Power))
	}
}

func (*graphHistory) add(values *[]float64, value float64) {
	if math.IsNaN(value) || math.IsInf(value, 0) || value < 0 {
		value = 0
	}
	if len(*values) >= maxGraphSamples {
		copy(*values, (*values)[1:])
		(*values)[len(*values)-1] = value
		return
	}
	*values = append(*values, value)
}

func (history *graphHistory) Render(
	sensor sensorData,
	width int,
	height int,
	layout graphLayout,
) string {
	definitions := []metricDefinition{
		{
			title:     "V.",
			unit:      "V",
			precision: 2,
			values:    &history.voltage,
			current: [3]float64{
				float64(sensor.VIN.Voltage),
				float64(sensor.Main.Voltage),
				float64(sensor.USB.Voltage),
			},
		},
		{
			title:     "A.",
			unit:      "A",
			precision: 3,
			values:    &history.current,
			current: [3]float64{
				float64(sensor.VIN.Current),
				float64(sensor.Main.Current),
				float64(sensor.USB.Current),
			},
		},
		{
			title:     "W.",
			unit:      "W",
			precision: 2,
			values:    &history.power,
			current: [3]float64{
				float64(sensor.VIN.Power),
				float64(sensor.Main.Power),
				float64(sensor.USB.Power),
			},
		},
	}

	layout = resolveGraphLayout(layout, width, height)
	if layout == graphLayoutHorizontal {
		panelHeight := metricGraphHeight(height, true)
		baseWidth := width / len(definitions)
		parts := make([]string, 0, len(definitions))
		used := 0
		for index, definition := range definitions {
			partWidth := baseWidth
			if index == len(definitions)-1 {
				partWidth = width - used
			}
			parts = append(parts, renderMetricGraph(
				definition,
				partWidth,
				panelHeight,
				false,
				true,
			))
			used += partWidth
		}
		return lipgloss.JoinHorizontal(lipgloss.Top, parts...)
	}

	panelHeight := metricGraphHeight(height/len(definitions), false)
	parts := make([]string, 0, len(definitions))
	for _, definition := range definitions {
		parts = append(parts, renderMetricGraph(
			definition,
			width,
			panelHeight,
			true,
			false,
		))
	}
	return lipgloss.JoinVertical(lipgloss.Left, parts...)
}

func resolveGraphLayout(layout graphLayout, width, height int) graphLayout {
	canRenderHorizontal := width >= minMetricGraphWidth*3 &&
		height >= minHorizontalMetricGraphHeight
	canRenderVertical := height >= minMetricGraphHeight*3

	switch layout {
	case graphLayoutHorizontal:
		if !canRenderHorizontal && canRenderVertical {
			return graphLayoutVertical
		}
		return graphLayoutHorizontal
	case graphLayoutVertical:
		if !canRenderVertical && canRenderHorizontal {
			return graphLayoutHorizontal
		}
		return graphLayoutVertical
	default:
		if width >= 108 || !canRenderVertical {
			return graphLayoutHorizontal
		}
		return graphLayoutVertical
	}
}

func metricGraphHeight(height int, legendBelow bool) int {
	minimumHeight := minMetricGraphHeight
	if legendBelow {
		minimumHeight = minHorizontalMetricGraphHeight
	}
	height = max(minimumHeight, height)

	// A vertical panel uses one title row, equally sized VIN/MAIN/USB plots,
	// and a two-row border. Horizontal layout also has a one-row legend.
	// Round down to 6/9/12... or 7/10/13... rows so every channel receives
	// the same graph height without padding rows.
	return height - (height-minimumHeight)%3
}

func renderMetricGraph(
	definition metricDefinition,
	width int,
	height int,
	stretch bool,
	legendBelow bool,
) string {
	width = max(minMetricGraphWidth, width)
	height = metricGraphHeight(height, legendBelow)

	panelStyle := lipgloss.NewStyle().
		Border(lipgloss.NormalBorder()).
		BorderForeground(lipgloss.Color("240"))
	contentWidth := max(1, width-panelStyle.GetHorizontalFrameSize())
	contentHeight := max(1, height-panelStyle.GetVerticalFrameSize())

	labelWidth := 15
	if contentWidth < 34 {
		labelWidth = 12
	}
	if legendBelow {
		labelWidth = 0
	} else {
		labelWidth = min(labelWidth, max(1, contentWidth-1))
	}
	plotWidth := max(1, contentWidth-labelWidth)

	maxValue := 1.0
	for _, values := range definition.values {
		start := max(0, len(values)-plotWidth)
		for _, value := range values[start:] {
			maxValue = max(maxValue, value)
		}
	}
	maxValue = math.Ceil(maxValue*10) / 10

	plotHeight := contentHeight - 1
	if legendBelow {
		plotHeight--
	}
	plotHeight = max(3, plotHeight)
	seriesHeight := max(1, plotHeight/3)
	names := [3]string{"VIN", "MAIN", "USB"}
	colors := [3]color.Color{
		lipgloss.Color("220"),
		lipgloss.Color("51"),
		lipgloss.Color("82"),
	}

	title := fmt.Sprintf("%s Scale 0..%.*f%s",
		definition.title, definition.precision, maxValue, definition.unit)
	lines := []string{lipgloss.NewStyle().
		Foreground(lipgloss.Color("244")).
		Render(ansi.Truncate(title, contentWidth, ""))}

	for index := range names {
		values := (*definition.values)[index]
		visible, padding := graphValues(values, plotWidth, stretch)

		rows := make([]strings.Builder, seriesHeight)
		for row := range rows {
			rows[row].Grow(contentWidth)
			label := ""
			if !legendBelow && row == seriesHeight/2 {
				label = fmt.Sprintf("%-4s %6.*f%s",
					names[index], definition.precision,
					definition.current[index], definition.unit)
			}
			label = ansi.Truncate(label, labelWidth, "")
			rows[row].WriteString(label)
			rows[row].WriteString(strings.Repeat(
				" ",
				max(0, labelWidth-ansi.StringWidth(label)),
			))
			rows[row].WriteString(strings.Repeat(" ", padding))
		}

		for _, value := range visible {
			scaled := value / maxValue * float64(seriesHeight)
			full := int(math.Floor(scaled))
			partial := scaled - float64(full)
			for row := 0; row < seriesHeight; row++ {
				level := seriesHeight - row - 1
				character := ' '
				switch {
				case level < full:
					character = '█'
				case level == full && partial > 0:
					character = partialBlock(partial)
				}
				rows[row].WriteRune(character)
			}
		}

		style := lipgloss.NewStyle().Foreground(colors[index])
		for row := range rows {
			lines = append(lines, style.Render(rows[row].String()))
		}
	}
	if legendBelow {
		lines = append(lines, renderMetricLegend(
			definition,
			names,
			colors,
			contentWidth,
		))
	}

	content := strings.Join(lines, "\n")
	return panelStyle.
		Width(width).
		Height(height).
		MaxWidth(width).
		MaxHeight(height).
		Render(content)
}

func renderMetricLegend(
	definition metricDefinition,
	names [3]string,
	colors [3]color.Color,
	width int,
) string {
	legend := func(displayNames [3]string, compact bool) string {
		parts := make([]string, 0, len(displayNames))
		for index, name := range displayNames {
			value := fmt.Sprintf(
				"%.*f",
				definition.precision,
				definition.current[index],
			)
			separator := " "
			if compact {
				if strings.HasPrefix(value, "0.") {
					value = value[1:]
				}
				separator = ":"
			}
			part := name + separator + value
			parts = append(parts, lipgloss.NewStyle().
				Foreground(colors[index]).
				Render(part))
		}
		joiner := "  "
		if compact {
			joiner = " "
		}
		return strings.Join(parts, joiner)
	}

	line := legend(names, false)
	if ansi.StringWidth(line) > width {
		line = legend([3]string{"V", "M", "U"}, true)
	}
	line = ansi.Truncate(line, width, "")
	return lipgloss.NewStyle().
		Width(width).
		Align(lipgloss.Center).
		Render(line)
}

func graphValues(
	values []float64,
	width int,
	stretch bool,
) ([]float64, int) {
	start := max(0, len(values)-width)
	visible := values[start:]
	if !stretch || len(visible) == 0 || len(visible) >= width {
		return visible, max(0, width-len(visible))
	}

	stretched := make([]float64, width)
	if len(visible) == 1 {
		for index := range stretched {
			stretched[index] = visible[0]
		}
		return stretched, 0
	}

	lastSource := float64(len(visible) - 1)
	lastTarget := float64(width - 1)
	for index := range stretched {
		position := float64(index) * lastSource / lastTarget
		left := int(math.Floor(position))
		right := min(left+1, len(visible)-1)
		fraction := position - float64(left)
		stretched[index] = visible[left] +
			(visible[right]-visible[left])*fraction
	}
	return stretched, 0
}

func partialBlock(value float64) rune {
	blocks := []rune{'▁', '▂', '▃', '▄', '▅', '▆', '▇', '█'}
	index := int(math.Ceil(value*float64(len(blocks)))) - 1
	index = max(0, min(index, len(blocks)-1))
	return blocks[index]
}
