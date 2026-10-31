# Unit: Spectre renderer
# Launched by build_win.ps1 under PowerShell 7 with -File; its stdout is
# replayed through Write-Host. Never dot-source this from PowerShell 5.1.

$env:IgnoreSpectreEncoding = $true
Import-Module PwshSpectreConsole

# Out-String renders plain text and drops colour, so write through a console
# that is told to emit ANSI even though stdout is redirected through PowerShell 5.1.
$consoleSettings = [Spectre.Console.AnsiConsoleSettings]::new()
$consoleSettings.Ansi = [Spectre.Console.AnsiSupport]::Yes
$consoleSettings.Out = [Spectre.Console.AnsiConsoleOutput]::new([Console]::Out)
$console = [Spectre.Console.AnsiConsole]::Create($consoleSettings)
$culture = [System.Globalization.CultureInfo]::InvariantCulture

function Format-ShortNumber {
    param([double]$Value)
    if ([math]::Abs($Value) -ge 10) { return $Value.ToString('0', $culture) }
    return $Value.ToString('0.0', $culture)
}

$action = $args[0]
switch ($action) {
    "step" {
        $ts = [Spectre.Console.Markup]::Escape($args[1])
        $text = [Spectre.Console.Markup]::Escape($args[2])
        Write-Host "$ts $text"
    }
    "rule" { Write-SpectreRule -Color Green3 }
    "chart" {
        $csvPath = $args[1]
        $rows = @(Import-Csv -Path $csvPath -Encoding UTF8)
        if ($rows.Count -eq 0) { return }

        $toolInfo = @(
            @{ Name = 'compiler'; Color = 'darkseagreen4'; Symbol = [char]0x2588 },
            @{ Name = 'linker';   Color = 'steelblue1';   Symbol = [char]0x2588 },
            @{ Name = 'build';    Color = 'orange3';    Symbol = [char]0x25A0 },  # Diamond symbol
            @{ Name = 'ccache';   Color = 'mediumpurple'; Symbol = [char]0x2588 }
        )
        $metrics = @(
            @{ Title = 'CPU %'; Prefix = 'cpu';   Total = 'build_cpu_percent'; Unit = '%' },
            @{ Title = 'CPU Cores'; Prefix = 'cores'; Total = 'build_cores';   Unit = '' },
            @{ Title = 'RAM';   Prefix = 'ram';   Total = 'build_rss_gb';      Unit = ' GB' }
        )

        # Average equal buckets so a long build still fits on one screen.
        $maxRows = 15
        $groups = @()
        if ($rows.Count -le $maxRows) {
            foreach ($row in $rows) { $groups += ,@($row) }
        } else {
            $size = [math]::Ceiling($rows.Count / $maxRows)
            for ($i = 0; $i -lt $rows.Count; $i += $size) {
                $end = [math]::Min($i + $size - 1, $rows.Count - 1)
                $groups += ,@($rows[$i..$end])
            }
        }

        $first = [datetime]$rows[0].timestamp
        $barWidth = [math]::Min(60, [math]::Max(20, $console.Profile.Width - 26))
        $blocks = @()

        foreach ($metric in $metrics) {
            $samples = foreach ($group in $groups) {
                $values = [ordered]@{}
                foreach ($tool in $toolInfo) {
                    $column = "$($metric.Prefix)_$($tool.Name)"
                    $sum = 0.0
                    foreach ($row in $group) { $sum += [double]$row.$column }
                    $values[$tool.Name] = $sum / $group.Count
                }
                $totalSum = 0.0
                foreach ($row in $group) { $totalSum += [double]$row.($metric.Total) }
                $total = $totalSum / $group.Count
                [pscustomobject]@{
                    Sec    = [int]((([datetime]$group[0].timestamp) - $first).TotalSeconds)
                    Values = $values
                    Total  = $total
                }
            }

            $peak = ($samples | Measure-Object -Property Total -Maximum).Maximum
            if ($peak -le 0) { continue }
            $peakText = Format-ShortNumber -Value $peak
            if ($blocks.Count -gt 0) { $blocks += [Spectre.Console.Markup]::new("") }
            $blocks += [Spectre.Console.Markup]::new("[bold]$($metric.Title)[/] [grey]peak $peakText$($metric.Unit)[/]")

            foreach ($sample in $samples) {
                if ($sample.Total -le 0) { continue }
                $chars = [math]::Min($barWidth, [math]::Max(1, [math]::Round($sample.Total / $peak * $barWidth)))
                $bar = ''
                $remaining = $chars
                $remainingValue = $sample.Total
                $segments = @($toolInfo | Where-Object { $sample.Values[$_.Name] -gt 0 })
                for ($i = 0; $i -lt $segments.Count; $i++) {
                    $tool = $segments[$i]
                    $value = $sample.Values[$tool.Name]
                    if ($i -eq $segments.Count - 1) {
                        $count = $remaining
                    } else {
                        $count = [math]::Round($value / $remainingValue * $remaining)
                        $count = [math]::Max(0, [math]::Min($count, $remaining))
                    }
                    if ($count -gt 0) { $bar += "[$($tool.Color)]" + ([string]$tool.Symbol * $count) + "[/]" }
                    $remaining -= $count
                    $remainingValue -= $value
                }
                $elapsed = "{0}:{1:00}" -f [math]::Floor($sample.Sec / 60), ($sample.Sec % 60)
                $valueText = Format-ShortNumber -Value $sample.Total
                # Pad to the full bar width so the closing rule stays in one column.
                $padding = ' ' * [math]::Max(0, $barWidth - $chars)
                $blocks += [Spectre.Console.Markup]::new(("[grey]{0,5}[/] {1}{2}[grey]{3}[/] {4,5}" -f $elapsed, $bar, $padding, [char]0x2502, $valueText))
            }
        }
        if ($blocks.Count -eq 0) { return }
        $blocks += [Spectre.Console.Markup]::new("[darkseagreen4]$([char]0x2588)[/] compiler  [steelblue1]$([char]0x2588)[/] linker  [orange3]$([char]0x25A0)[/] build  [mediumpurple]$([char]0x2588)[/] ccache")
        $content = [Spectre.Console.Rows]::new([Spectre.Console.Rendering.IRenderable[]]$blocks)
        $panel = $content | Format-SpectrePanel -Header "Resource use" -Color Grey -Expand
        $console.Write($panel)
    }
    "textpanel" {
        $source = $args[1]
        $header = $args[2]
        if (-not (Test-Path $source)) { return }
        $blocks = @()
        foreach ($line in @(Get-Content -LiteralPath $source)) {
            # ccache colourises its own output; strip the escapes and keep the text.
            $clean = [regex]::Replace($line, "\x1b\[[0-9;]*m", "")
            $blocks += [Spectre.Console.Markup]::new([Spectre.Console.Markup]::Escape($clean))
        }
        if ($blocks.Count -eq 0) { return }
        $content = [Spectre.Console.Rows]::new([Spectre.Console.Rendering.IRenderable[]]$blocks)
        $panel = $content | Format-SpectrePanel -Header $header -Color Grey -Expand
        $console.Write($panel)
    }
    "panel" {
        $lines = @(Get-Content $args[1])
        $total     = [Spectre.Console.Markup]::Escape("Total:     $($lines[0])")
        $configure = "[grey]" + [Spectre.Console.Markup]::Escape("Configure: $($lines[1])") + "[/]"
        $build     = "[grey]" + [Spectre.Console.Markup]::Escape("Build:     $($lines[2])") + "[/]"
        $data = @{
            Value = $total
            Children = @(
                @{ Value = $configure; Children = @() }
                @{ Value = $build; Children = @() }
            )
        }
        $tree = Format-SpectreTree -Data $data
        # Grey style colours the tree guides; the node text keeps its own markup.
        $tree.Style = [Spectre.Console.Style]::new([Spectre.Console.Color]::Grey)
        $panel = $tree | Format-SpectrePanel -Header "Build complete!" -Color Green3 -Expand
        $console.Write($panel)
    }
}
