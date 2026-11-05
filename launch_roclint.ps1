Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;

public class DesktopProcessLauncher {
    [StructLayout(LayoutKind.Sequential)]
    public struct STARTUPINFO {
        public int cb;
        public string lpReserved;
        public string lpDesktop;
        public string lpTitle;
        public int dwX;
        public int dwY;
        public int dwXSize;
        public int dwYSize;
        public int dwXCountChars;
        public int dwYCountChars;
        public int dwFillAttribute;
        public int dwFlags;
        public short wShowWindow;
        public short cbReserved2;
        public IntPtr lpReserved2;
        public IntPtr hStdInput;
        public IntPtr hStdOutput;
        public IntPtr hStdError;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct PROCESS_INFORMATION {
        public IntPtr hProcess;
        public IntPtr hThread;
        public int dwProcessId;
        public int dwThreadId;
    }

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Auto)]
    public static extern bool CreateProcess(
        string lpApplicationName,
        string lpCommandLine,
        IntPtr lpProcessAttributes,
        IntPtr lpThreadAttributes,
        bool bInheritHandles,
        uint dwCreationFlags,
        IntPtr lpEnvironment,
        string lpCurrentDirectory,
        ref STARTUPINFO lpStartupInfo,
        out PROCESS_INFORMATION lpProcessInformation);

    [DllImport("kernel32.dll")]
    public static extern bool CloseHandle(IntPtr handle);

    public static int Start(string appPath, string args, string workingDir) {
        STARTUPINFO si = new STARTUPINFO();
        si.cb = Marshal.SizeOf(si);
        si.lpDesktop = @"WinSta0\Default";

        PROCESS_INFORMATION pi;
        string fullCmd = "\"" + appPath + "\" " + args;
        bool ok = CreateProcess(null, fullCmd, IntPtr.Zero, IntPtr.Zero, false, 0x00000208u, IntPtr.Zero, workingDir, ref si, out pi);
        if (!ok) {
            int err = Marshal.GetLastWin32Error();
            Console.WriteLine("CreateProcess failed with error: " + err);
            return -1;
        }
        Console.WriteLine("Successfully launched PID " + pi.dwProcessId + " on interactive desktop WinSta0\\Default");
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return pi.dwProcessId;
    }
}
'@

# Clean any existing lock file
$root = $PSScriptRoot
$lockPath = Join-Path $env:LOCALAPPDATA "roclint\Roclint\roclint-host.lock"
if (Test-Path $lockPath) {
    Remove-Item $lockPath -Force -ErrorAction SilentlyContinue
    Write-Host "Cleaned stale lock file: $lockPath"
}

$deploy = Join-Path $root "build\Release\bin\roclint_host.exe"
$fallback = Join-Path $root "build\Release\roclint_host.exe"
$qtBin = "D:\Qt\6.8.2\msvc2022_64\bin"

if (Test-Path $deploy) {
    $app = $deploy
    $workingDir = Join-Path $root "build\Release\bin"
} else {
    Write-Host "[WARN] Self-contained deployment is unavailable; using the Qt kit on PATH."
    $env:PATH = $qtBin + [IO.Path]::PathSeparator + $env:PATH
    $app = $fallback
    $workingDir = Join-Path $root "build\Release"
}
$args = "--show --pin --workspace `"$root`""

$targetPid = [DesktopProcessLauncher]::Start($app, $args, $workingDir)
Write-Host "Launched Roclint with PID: $targetPid"
