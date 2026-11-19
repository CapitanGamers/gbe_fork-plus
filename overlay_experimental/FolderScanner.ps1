<#
    .SYNOPSIS
        Folder Scanner Script for Windows 10

    .DESCRIPTION
        - Scans all files in the folder where this script is located
        - Includes all subfolders recursively
        - Reads text-based files and embeds their contents
        - Marks binary / encrypted / non-readable files as unreadable
          with the reason
        - Outputs a single UTF-8 (BOM) .txt report next to this script
        - UTF-8 with BOM is used so Notepad shows Persian / Arabic / CJK
          text correctly without manual encoding selection.

    .NOTES
        Run from PowerShell:
            powershell -ExecutionPolicy Bypass -File .\FolderScanner.ps1
        Or after allowing scripts once:
            Set-ExecutionPolicy -Scope CurrentUser RemoteSigned
#>

# =====================================================================
#  Configuration
# =====================================================================

# Determine the directory where this script lives
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

# Output file path (next to this script)
$OutputFile = Join-Path $ScriptDir "Folder_Contents_Report.txt"

# Max characters of a single file's content to embed in the report
# (very large text files are truncated to keep the report manageable)
$MaxContentChars = 200000

# Extensions explicitly treated as UNREADABLE per user request
# (treated as "encrypted / non-readable" even though they may be text)
$UnreadableByUser = @(
    '.bat', '.cmd', '.com', '.scr', '.shb', '.shs', '.lnk'
)

# Common BINARY extensions (cannot be displayed as text)
$BinaryExtensions = @(
    '.exe','.dll','.sys','.drv','.ocx','.cpl','.so','.dylib','.bin','.dat',
    '.db','.sqlite','.sqlite3','.mdb','.accdb','.class','.jar','.war','.ear',
    '.pyc','.pyo','.pyd','.wasm','.msi','.msp','.msm','.obj','.lib','.a',
    '.zip','.rar','.7z','.tar','.gz','.bz2','.xz','.lz','.lzma','.cab',
    '.iso','.img','.vhd','.vhdx','.vmdk','.dmg','.pkg','.deb','.rpm','.zst',
    '.jpg','.jpeg','.png','.gif','.bmp','.tiff','.tif','.webp','.ico','.cur',
    '.ani','.jp2','.j2k','.heic','.heif','.avif','.psd','.ai','.eps','.svgz',
    '.tga','.dds','.raw','.cr2','.nef','.arw','.dng','.orf','.rw2','.hdr','.exr',
    '.mp3','.wav','.flac','.aac','.ogg','.oga','.opus','.wma','.m4a','.aiff',
    '.aif','.au','.snd','.mid','.midi','.mp4','.mkv','.avi','.mov','.wmv',
    '.flv','.webm','.mpeg','.mpg','.mpe','.m2ts','.mts','.3gp','.3g2','.ogv',
    '.vob','.rm','.rmvb','.asf','.f4v','.swf','.pdf','.doc','.docx','.xls',
    '.xlsx','.ppt','.pptx','.odt','.ods','.odp','.chm','.hlp','.key','.numbers',
    '.pages','.ps','.xps','.oxps','.dwg','.dxf','.stl','.fbx','.dae','.3ds',
    '.blend','.max','.ttf','.otf','.woff','.woff2','.eot','.fon','.pdb','.prc',
    '.mobi','.azw','.azw3','.epub','.lit','.sami','.smi','.pcap','.pcapng'
)

# Text-readable extensions (whitelist used as a hint, not strictly enforced)
$TextExtensions = @(
    '.txt','.log','.csv','.tsv','.json','.xml','.html','.htm','.css','.js',
    '.jsx','.ts','.tsx','.mjs','.cjs','.py','.java','.c','.cpp','.cc','.cxx',
    '.h','.hpp','.hxx','.cs','.vb','.ps1','.psm1','.psd1','.sh','.bash','.zsh',
    '.rb','.php','.pl','.pm','.sql','.md','.markdown','.rst','.ini','.cfg',
    '.config','.conf','.yml','.yaml','.toml','.env','.gitignore','.gitattributes',
    '.editorconfig','.svg','.asmx','.ascx','.razor','.vue','.svelte','.go','.rs',
    '.kt','.kts','.swift','.dart','.lua','.r','.m','.mm','.scala','.clj','.cljs',
    '.edn','.ex','.exs','.erl','.hrl','.fs','.fsx','.fsi','.vbproj','.csproj',
    '.sln','.props','.targets','.gradle','.mf','.tex','.bib','.nfo','.srt',
    '.vtt','.ass','.tab','.http','.rest','.graphql','.gql','.proto','.thrift',
    '.cmake','.dockerignore','.npmignore','.reg','.diff','.patch','.ini','.ldif'
)

# =====================================================================
#  Helper functions
# =====================================================================

function Test-BinaryByContent {
    # Returns $true if the file looks binary (contains NUL bytes in first 8 KB)
    param([string]$Path)
    try {
        $stream = [System.IO.File]::OpenRead($Path)
        try {
            $buffer = New-Object byte[] 8192
            $read = $stream.Read($buffer, 0, 8192)
            for ($i = 0; $i -lt $read; $i++) {
                if ($buffer[$i] -eq 0) { return $true }
            }
            return $false
        } finally {
            $stream.Close()
        }
    } catch {
        return $true
    }
}

function Read-FileWithEncoding {
    # Reads file bytes and decodes them as text, honoring BOM if present.
    # Falls back to system default encoding if UTF-8 strict decode fails.
    param([string]$Path)
    try {
        $bytes = [System.IO.File]::ReadAllBytes($Path)
        if ($bytes.Length -eq 0) { return "" }

        # UTF-8 BOM
        if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
            $enc = New-Object System.Text.UTF8Encoding($false, $false)
            return $enc.GetString($bytes, 3, $bytes.Length - 3)
        }
        # UTF-16 LE BOM
        if ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) {
            $enc = New-Object System.Text.UnicodeEncoding($false, $false)
            return $enc.GetString($bytes, 2, $bytes.Length - 2)
        }
        # UTF-16 BE BOM
        if ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFE -and $bytes[1] -eq 0xFF) {
            $enc = New-Object System.Text.UnicodeEncoding($true, $false)
            return $enc.GetString($bytes, 2, $bytes.Length - 2)
        }
        # UTF-32 LE BOM
        if ($bytes.Length -ge 4 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE -and $bytes[2] -eq 0 -and $bytes[3] -eq 0) {
            $enc = New-Object System.Text.UTF32Encoding($false, $false)
            return $enc.GetString($bytes, 4, $bytes.Length - 4)
        }

        # No BOM: try strict UTF-8 first, fall back to system codepage
        $utf8Strict = New-Object System.Text.UTF8Encoding($false, $true)
        try {
            return $utf8Strict.GetString($bytes)
        } catch {
            return [System.Text.Encoding]::Default.GetString($bytes)
        }
    } catch {
        return $null
    }
}

function Get-FolderTree {
    # Builds a tree-style listing of folders and files.
    param([string]$Root)
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.AppendLine($Root)

    function Render-Dir {
        param([string]$Path, [string]$Prefix)
        $dirs  = Get-ChildItem -Path $Path -Directory -Force -ErrorAction SilentlyContinue | Sort-Object Name
        $files = Get-ChildItem -Path $Path -File -Force -ErrorAction SilentlyContinue | Sort-Object Name

        $items = @()
        foreach ($d in $dirs)  { $items += @{ Name = $d.Name; IsDir = $true;  FullPath = $d.FullName } }
        foreach ($f in $files) { $items += @{ Name = $f.Name; IsDir = $false; FullPath = $f.FullName } }

        for ($i = 0; $i -lt $items.Count; $i++) {
            $isLast    = ($i -eq $items.Count - 1)
            $connector = if ($isLast) { "\__ " } else { "|-- " }
            $marker    = if ($items[$i].IsDir) { "[DIR]  " } else { "[FILE] " }
            [void]$sb.AppendLine($Prefix + $connector + $marker + $items[$i].Name)
            if ($items[$i].IsDir) {
                $newPrefix = $Prefix + $(if ($isLast) { "    " } else { "|   " })
                Render-Dir -Path $items[$i].FullPath -Prefix $newPrefix
            }
        }
    }

    Render-Dir -Path $Root -Prefix ""
    return $sb.ToString()
}

function Format-Bytes {
    param([long]$Bytes)
    if ($Bytes -ge 1GB) { return ("{0:N2} GB" -f ($Bytes / 1GB)) }
    if ($Bytes -ge 1MB) { return ("{0:N2} MB" -f ($Bytes / 1MB)) }
    if ($Bytes -ge 1KB) { return ("{0:N2} KB" -f ($Bytes / 1KB)) }
    return "$Bytes B"
}

# =====================================================================
#  Main
# =====================================================================

# UTF-8 with BOM so Notepad shows Persian / Arabic / CJK text correctly
$utf8WithBom = New-Object System.Text.UTF8Encoding($true)
$writer = New-Object System.IO.StreamWriter($OutputFile, $false, $utf8WithBom)

$totalFiles     = 0
$readableCount  = 0
$unreadableCount = 0

try {
    $writer.WriteLine("================================================================")
    $writer.WriteLine("  FOLDER CONTENTS REPORT")
    $writer.WriteLine("  Generated     : " + (Get-Date).ToString("yyyy-MM-dd HH:mm:ss"))
    $writer.WriteLine("  Source folder : $ScriptDir")
    $writer.WriteLine("  Output file   : $OutputFile")
    $writer.WriteLine("================================================================")
    $writer.WriteLine("")

    # ---------------- Section 1: Folder structure ----------------
    $writer.WriteLine("================================================================")
    $writer.WriteLine("  SECTION 1: FOLDER STRUCTURE")
    $writer.WriteLine("================================================================")
    $writer.WriteLine("")
    $tree = Get-FolderTree -Root $ScriptDir
    $writer.WriteLine($tree)
    $writer.WriteLine("")

    # ---------------- Section 2: File contents -------------------
    $writer.WriteLine("================================================================")
    $writer.WriteLine("  SECTION 2: FILE CONTENTS")
    $writer.WriteLine("================================================================")
    $writer.WriteLine("")

    $allFiles = Get-ChildItem -Path $ScriptDir -File -Recurse -Force -ErrorAction SilentlyContinue |
                Sort-Object FullName

    # Exclude the report file itself (and any previously generated reports)
    $allFiles = $allFiles | Where-Object {
        $_.FullName -ne $OutputFile -and
        $_.Name -notmatch '^Folder_Contents_Report.*\.txt$'
    }

    $totalFiles = @($allFiles).Count
    $currentIndex = 0

    foreach ($file in $allFiles) {
        $currentIndex++
        $relativePath = $file.FullName.Substring($ScriptDir.Length).TrimStart('\')
        $ext = $file.Extension.ToLower()
        $sizeStr = Format-Bytes -Bytes $file.Length

        $writer.WriteLine("----------------------------------------------------------------")
        $writer.WriteLine(("FILE #{0} of {1}" -f $currentIndex, $totalFiles))
        $writer.WriteLine("Path      : $relativePath")
        $writer.WriteLine("Size      : $sizeStr ($($file.Length) bytes)")
        $writer.WriteLine("Modified  : " + $file.LastWriteTime.ToString("yyyy-MM-dd HH:mm:ss"))
        $writer.WriteLine("Extension : $ext")
        $writer.WriteLine("----------------------------------------------------------------")

        # ---- Decide readability ----
        if ($UnreadableByUser -contains $ext) {
            $writer.WriteLine(">> UNREADABLE: This file type ($ext) is treated as encrypted/non-readable by policy.")
            $writer.WriteLine(">> Reason    : User-configured as non-readable (script / command / shortcut file).")
            $writer.WriteLine("")
            $unreadableCount++
            continue
        }

        if ($BinaryExtensions -contains $ext) {
            $writer.WriteLine(">> UNREADABLE: This is a binary file ($ext).")
            $writer.WriteLine(">> Reason    : Binary format - cannot be displayed as text.")
            $writer.WriteLine("")
            $unreadableCount++
            continue
        }

        if (Test-BinaryByContent -Path $file.FullName) {
            $writer.WriteLine(">> UNREADABLE: File contains NUL bytes or non-text data (likely binary).")
            $writer.WriteLine(">> Reason    : Binary content detected via byte-scan (first 8 KB).")
            $writer.WriteLine("")
            $unreadableCount++
            continue
        }

        $content = Read-FileWithEncoding -Path $file.FullName
        if ($null -eq $content) {
            $writer.WriteLine(">> UNREADABLE: Failed to read file content (access denied or locked).")
            $writer.WriteLine(">> Reason    : I/O error while reading.")
            $writer.WriteLine("")
            $unreadableCount++
            continue
        }

        if ($content.Length -eq 0) {
            $writer.WriteLine(">> EMPTY FILE (0 bytes of text content).")
            $writer.WriteLine("")
            $readableCount++
            continue
        }

        if ($content.Length -gt $MaxContentChars) {
            $writer.WriteLine(">> NOTE: File content truncated to first $MaxContentChars characters (total: $($content.Length) chars).")
            $writer.WriteLine("")
            $writer.WriteLine("--- BEGIN CONTENT (truncated) ---")
            $writer.WriteLine($content.Substring(0, $MaxContentChars))
            $writer.WriteLine("--- END CONTENT (truncated) ---")
        } else {
            $writer.WriteLine("--- BEGIN CONTENT ---")
            $writer.WriteLine($content)
            $writer.WriteLine("--- END CONTENT ---")
        }
        $writer.WriteLine("")
        $readableCount++
    }

    # ---------------- Section 3: Summary -------------------------
    $writer.WriteLine("================================================================")
    $writer.WriteLine("  SUMMARY")
    $writer.WriteLine("================================================================")
    $writer.WriteLine(("Total files scanned : {0}" -f $totalFiles))
    $writer.WriteLine(("Readable files      : {0}" -f $readableCount))
    $writer.WriteLine(("Unreadable files    : {0}" -f $unreadableCount))
    $writer.WriteLine(("Report location     : {0}" -f $OutputFile))
    $writer.WriteLine("================================================================")

} finally {
    $writer.Close()
}

# Console feedback
Write-Host ""
Write-Host "Report generated successfully:" -ForegroundColor Green
Write-Host "  $OutputFile" -ForegroundColor Cyan
Write-Host ""
Write-Host ("Total files scanned : {0}" -f $totalFiles)
Write-Host ("Readable            : {0}" -f $readableCount)
Write-Host ("Unreadable          : {0}" -f $unreadableCount)
Write-Host ""
