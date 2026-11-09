# Determine VS generator from install path (like build_win.bat does)
        # Path is like C:\Program Files\Microsoft Visual Studio\2026\Enterprise
        # Match \18\ pattern (backslash, version digits, backslash)
        $vsMajor = if ($vsInstall -match '\\\\\\\(1[6-9]|[2-9][0-9]\\\)\\\\'') { $matches[0].Trim('\\') } else { "17" }

