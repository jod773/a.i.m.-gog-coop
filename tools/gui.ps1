# Screenshot + input helper for testing the game.
#   gui.ps1 shot <out.png> [scale]
#   gui.ps1 click <x> <y>          (screen coordinates)
#   gui.ps1 key <vk> [count]       (virtual key code, decimal)
#   gui.ps1 rect                   (prints the AIMII window rectangle)
param([string]$cmd, [string]$a1, [string]$a2, [string]$a3)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class W {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, IntPtr e);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint f, IntPtr e);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint c, uint t);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  public static IntPtr FindByPid(uint pid) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p);
      if (p == pid && IsWindowVisible(h)) { found = h; return false; } return true; }, IntPtr.Zero);
    return found;
  }
}
"@
function GameWnd {
  $p = Get-Process AIMII, AIM -ErrorAction SilentlyContinue | Select-Object -First 1
  if (-not $p) { return [IntPtr]::Zero }
  return [W]::FindByPid([uint32]$p.Id)
}
# Input is only ever sent when the game window exists and is in front;
# otherwise the script aborts so nothing reaches other applications.
function RequireGameFront {
  $h = GameWnd
  if ($h -eq [IntPtr]::Zero) { Write-Output "ABORT: game window not found"; exit 2 }
  [W]::SetForegroundWindow($h) | Out-Null
  Start-Sleep -Milliseconds 200
  if ([W]::GetForegroundWindow() -ne $h) { Write-Output "ABORT: game window is not in front"; exit 3 }
  return $h
}
switch ($cmd) {
  'shot' {
    $h = GameWnd
    $b = [System.Windows.Forms.Screen]::PrimaryScreen
    Add-Type -AssemblyName System.Windows.Forms
    $r = New-Object W+RECT
    if ($h -ne [IntPtr]::Zero) {
      [W]::GetClientRect($h, [ref]$r) | Out-Null
      $pt = New-Object W+POINT; [W]::ClientToScreen($h, [ref]$pt) | Out-Null
      $x = $pt.X; $y = $pt.Y; $w = $r.R; $hh = $r.B
    } else {
      Write-Output "ABORT: game window not found (not capturing the desktop)"; exit 2
    }
    $bmp = New-Object System.Drawing.Bitmap $w, $hh
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($x, $y, 0, 0, $bmp.Size)
    $sc = 1.0; if ($a2) { $sc = [double]$a2 }
    if ($sc -ne 1.0) {
      $b2 = New-Object System.Drawing.Bitmap ([int]($w * $sc)), ([int]($hh * $sc))
      $g2 = [System.Drawing.Graphics]::FromImage($b2); $g2.InterpolationMode = 'HighQualityBicubic'
      $g2.DrawImage($bmp, 0, 0, $b2.Width, $b2.Height); $bmp = $b2
    }
    $bmp.Save($a1, [System.Drawing.Imaging.ImageFormat]::Png)
    "saved $a1 origin=($x,$y) size=${w}x${hh}"
  }
  'rect' {
    $h = GameWnd; $r = New-Object W+RECT; [W]::GetClientRect($h, [ref]$r) | Out-Null
    $pt = New-Object W+POINT; [W]::ClientToScreen($h, [ref]$pt) | Out-Null
    "origin=($($pt.X),$($pt.Y)) size=$($r.R)x$($r.B) hwnd=$h"
  }
  'click' {
    # coordinates are relative to the game client area
    $h = RequireGameFront
    $r = New-Object W+RECT; [W]::GetClientRect($h, [ref]$r) | Out-Null
    if ([int]$a1 -lt 0 -or [int]$a2 -lt 0 -or [int]$a1 -ge $r.R -or [int]$a2 -ge $r.B) { "ABORT: outside game window"; exit 4 }
    $pt = New-Object W+POINT; [W]::ClientToScreen($h, [ref]$pt) | Out-Null
    [W]::SetCursorPos($pt.X + [int]$a1, $pt.Y + [int]$a2) | Out-Null
    Start-Sleep -Milliseconds 150
    [W]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 80
    [W]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)
    "clicked $a1,$a2"
  }
  'move' {
    $h = RequireGameFront; $pt = New-Object W+POINT; [W]::ClientToScreen($h, [ref]$pt) | Out-Null
    [W]::SetCursorPos($pt.X + [int]$a1, $pt.Y + [int]$a2) | Out-Null
  }
  'key' {
    $h = RequireGameFront
    $n = 1; if ($a2) { $n = [int]$a2 }
    for ($i = 0; $i -lt $n; $i++) {
      $sc = [byte][W]::MapVirtualKey([uint32]$a1, 0)
      [W]::keybd_event([byte]$a1, $sc, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 60
      [W]::keybd_event([byte]$a1, $sc, 2, [IntPtr]::Zero); Start-Sleep -Milliseconds 120
    }
    "key $a1 x$n"
  }
}
