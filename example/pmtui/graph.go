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
	scale     [4]float64
	values    *[3][]float64
	current   [3]float64
}

type brailleCell struct {
	dots uint8
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
	darkBackground bool,
) string {
	lightDark := lipgloss.LightDark(darkBackground)
	colors := [3]color.Color{
		lightDark(lipgloss.Color("#dc3545"), lipgloss.Color("#d16969")),
		lightDark(lipgloss.Color("#198754"), lipgloss.Color("#4ec9b0")),
		lightDark(lipgloss.Color("#0d6efd"), lipgloss.Color("#569cd6")),
	}
	definitions := []metricDefinition{
		{
			title:     "V.",
			unit:      "V",
			precision: 2,
			scale:     [4]float64{5, 10, 15, 25},
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
			scale:     [4]float64{1, 2.5, 5, 10},
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
			scale:     [4]float64{5, 20, 50, 160},
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
				colors,
			))
			used += partWidth
		}
		return lipgloss.JoinHorizontal(lipgloss.Top, parts...)
	}

	parts := make([]string, 0, len(definitions))
	baseHeight := height / len(definitions)
	used := 0
	for index, definition := range definitions {
		partHeight := baseHeight
		if index == len(definitions)-1 {
			partHeight = height - used
		}
		parts = append(parts, renderMetricGraph(
			definition,
			width,
			metricGraphHeight(partHeight, false),
			true,
			false,
			colors,
		))
		used += partHeight
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

	// Grow in three-row steps so VIN, MAIN, and USB always receive the same
	// Braille plot height. Keep a one- or two-row remainder outside the metric
	// panels instead of distorting a single channel.
	return height - (height-minimumHeight)%3
}

func renderMetricGraph(
	definition metricDefinition,
	width int,
	height int,
	stretch bool,
	legendBelow bool,
	colors [3]color.Color,
) string {
	width = max(minMetricGraphWidth, width)
	height = max(minMetricGraphHeight, height)

	panelStyle := lipgloss.NewStyle().
		Border(lipgloss.NormalBorder()).
		BorderForeground(lipgloss.Color("240"))
	contentWidth := max(1, width-panelStyle.GetHorizontalFrameSize())
	contentHeight := max(1, height-panelStyle.GetVerticalFrameSize())

	axisWidth := 1
	labelWidth := 15
	if contentWidth < 34 {
		labelWidth = 12
	}
	if legendBelow {
		labelWidth = 0
	} else {
		labelWidth = min(
			labelWidth,
			max(0, contentWidth-axisWidth-1),
		)
	}
	plotWidth := max(1, contentWidth-labelWidth-axisWidth)
	logicalWidth := plotWidth * 2
	maxValue := metricScale(definition, logicalWidth)
	plotHeight := contentHeight - 1
	if legendBelow {
		plotHeight--
	}
	plotHeight = max(3, plotHeight)
	seriesHeight := plotHeight / 3
	names := [3]string{"VIN", "MAIN", "USB"}

	title := fmt.Sprintf("%s Scale 0..%.*f%s",
		definition.title, definition.precision, maxValue, definition.unit)
	lines := []string{lipgloss.NewStyle().
		Foreground(lipgloss.Color("244")).
		Render(ansi.Truncate(title, contentWidth, ""))}

	for index := range names {
		plot := make([][]brailleCell, seriesHeight)
		for row := range plot {
			plot[row] = make([]brailleCell, plotWidth)
		}
		visible, padding := graphValues(
			(*definition.values)[index],
			logicalWidth,
			stretch,
		)
		drawBrailleSeries(
			plot,
			visible,
			padding,
			maxValue,
		)

		for row := range plot {
			prefix := ""
			if !legendBelow {
				label := ""
				if row == (seriesHeight-1)/2 {
					label = fmt.Sprintf(
						"%-4s %6.*f%s",
						names[index],
						definition.precision,
						definition.current[index],
						definition.unit,
					)
				}
				label = ansi.Truncate(label, labelWidth, "")
				prefix = lipgloss.NewStyle().
					Foreground(colors[index]).
					Render(label)
				prefix += strings.Repeat(
					" ",
					max(0, labelWidth-ansi.StringWidth(label)),
				)
			}
			axis := '┤'
			if row == seriesHeight-1 {
				axis = '┼'
			}
			prefix += lipgloss.NewStyle().
				Foreground(lipgloss.Color("244")).
				Render(string(axis))
			lines = append(
				lines,
				prefix+renderBrailleRow(plot[row], colors[index]),
			)
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

func metricScale(definition metricDefinition, width int) float64 {
	maximum := 0.0
	for _, values := range definition.values {
		start := max(0, len(values)-max(1, width))
		for _, value := range values[start:] {
			maximum = max(maximum, value)
		}
	}
	for _, scale := range definition.scale {
		if maximum <= scale {
			return scale
		}
	}
	return definition.scale[len(definition.scale)-1]
}

func drawBrailleSeries(
	plot [][]brailleCell,
	values []float64,
	padding int,
	maximum float64,
) {
	if len(plot) == 0 || len(plot[0]) == 0 || len(values) == 0 {
		return
	}

	pixelWidth := len(plot[0]) * 2
	pixelHeight := len(plot) * 4
	pointY := func(value float64) int {
		value = max(0, min(value, maximum))
		if pixelHeight == 1 || maximum <= 0 {
			return 0
		}
		scaled := value / maximum * float64(pixelHeight-1)
		return pixelHeight - 1 - int(math.Round(scaled))
	}
	setPixel := func(x, y int) {
		if x < 0 || x >= pixelWidth || y < 0 || y >= pixelHeight {
			return
		}
		plot[y/4][x/2].dots |= brailleDot(x%2, y%4)
	}

	if len(values) == 1 {
		setPixel(padding, pointY(values[0]))
		return
	}

	for index := 0; index < len(values)-1; index++ {
		drawBrailleLine(
			padding+index,
			pointY(values[index]),
			padding+index+1,
			pointY(values[index+1]),
			setPixel,
		)
	}
}

func brailleDot(column, row int) uint8 {
	bits := [4][2]uint8{
		{1 << 0, 1 << 3},
		{1 << 1, 1 << 4},
		{1 << 2, 1 << 5},
		{1 << 6, 1 << 7},
	}
	return bits[row][column]
}

func drawBrailleLine(x0, y0, x1, y1 int, setPixel func(int, int)) {
	dx := absInt(x1 - x0)
	sx := 1
	if x0 > x1 {
		sx = -1
	}
	dy := -absInt(y1 - y0)
	sy := 1
	if y0 > y1 {
		sy = -1
	}
	err := dx + dy

	for {
		setPixel(x0, y0)
		if x0 == x1 && y0 == y1 {
			return
		}
		twiceError := 2 * err
		if twiceError >= dy {
			err += dy
			x0 += sx
		}
		if twiceError <= dx {
			err += dx
			y0 += sy
		}
	}
}

func absInt(value int) int {
	if value < 0 {
		return -value
	}
	return value
}

func renderBrailleRow(
	row []brailleCell,
	foreground color.Color,
) string {
	var line strings.Builder
	for _, cell := range row {
		if cell.dots == 0 {
			line.WriteByte(' ')
		} else {
			line.WriteRune(rune(0x2800) + rune(cell.dots))
		}
	}
	return lipgloss.NewStyle().Foreground(foreground).Render(line.String())
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
