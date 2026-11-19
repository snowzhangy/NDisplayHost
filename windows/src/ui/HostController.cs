using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.IO.Pipes;
using System.Linq;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;

namespace WvdHost {
  public sealed class HostOptions {
    public string Display = "";
    public string Encoder = "auto";
    public int Fps = 60, Bitrate = 60000, Port = 50001;
    public bool AllowInput, Synthetic, ScreenOff, NoAudio;
  }

  // Every process is a child of this UI's kill-on-close job. Normal stops use
  // an event: media releases/disarms input before the helper itself shuts down.
  public sealed class HostController : IDisposable {
    public event Action<string> Line;
    public readonly string Root;
    private readonly IntPtr job;
    private Process media, input, receiver, card, network;
    private EventWaitHandle mediaStop, inputStop, networkStop;
    private string pipeName;
    private ulong token;
    private bool disposed;
    public int Port { get; private set; }
    public bool Running { get { return Alive(media); } }
    public bool ReceiverRunning { get { return Alive(receiver); } }
    public bool NetworkRunning { get { return Alive(network); } }
    public bool InputAvailable { get { return Alive(input); } }

    public HostController() {
      Root = Path.GetFullPath(Path.Combine(AppDomain.CurrentDomain.BaseDirectory, ".."));
      job = CreateJobObject(IntPtr.Zero, null);
      var limits = new JobLimits();
      limits.BasicLimitInformation.LimitFlags = 0x2000; // KILL_ON_JOB_CLOSE
      int size = Marshal.SizeOf(typeof(JobLimits));
      IntPtr memory = Marshal.AllocHGlobal(size);
      try {
        Marshal.StructureToPtr(limits, memory, false);
        if (job == IntPtr.Zero || !SetInformationJobObject(job, 9, memory, (uint)size))
          throw new System.ComponentModel.Win32Exception();
      } finally { Marshal.FreeHGlobal(memory); }
      Port = 50001;
    }
    public static bool Alive(Process process) {
      try { return process != null && !process.HasExited; } catch { return false; }
    }
    private void Emit(string line) { var handler = Line; if (handler != null) handler(line); }
    public static string Quote(string value) {
      // CommandLineToArgvW quoting, including embedded quotes/trailing slashes.
      var result = new StringBuilder("\""); int slashes = 0;
      foreach (char c in value) {
        if (c == '\\') { ++slashes; continue; }
        if (c == '"') { result.Append('\\', slashes * 2 + 1).Append(c); slashes = 0; }
        else { result.Append('\\', slashes).Append(c); slashes = 0; }
      }
      return result.Append('\\', slashes * 2).Append('"').ToString();
    }
    private string Exe(string relative) { return Path.Combine(Root, relative); }
    private Process Spawn(string relative, IEnumerable<string> args, bool captureOutput) {
      string executable = Exe(relative);
      if (!File.Exists(executable)) throw new FileNotFoundException("Build component missing: " + relative);
      var info = new ProcessStartInfo(executable, String.Join(" ", args.Select(Quote))) {
        UseShellExecute = false, CreateNoWindow = true,
        RedirectStandardError = true, RedirectStandardOutput = true,
        RedirectStandardInput = true, WorkingDirectory = Root,
        StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8
      };
      var process = new Process { StartInfo = info };
      process.ErrorDataReceived += delegate(object sender, DataReceivedEventArgs e) {
        if (!String.IsNullOrEmpty(e.Data)) Emit(e.Data);
      };
      process.OutputDataReceived += delegate(object sender, DataReceivedEventArgs e) {
        // The injector prints its consent token on stdout: always discard it.
        if (captureOutput && !String.IsNullOrEmpty(e.Data)) Emit(e.Data);
      };
      process.Start();
      if (!AssignProcessToJobObject(job, process.Handle) && !process.HasExited) {
        try { process.Kill(); } catch { }
        process.Dispose(); throw new InvalidOperationException("Cannot contain the child process.");
      }
      process.BeginErrorReadLine(); process.BeginOutputReadLine();
      return process;
    }
    private static string NewStopEvent(out EventWaitHandle signal) {
      string name = "Local\\WvdUiStop-" + Guid.NewGuid().ToString("N");
      signal = new EventWaitHandle(false, EventResetMode.ManualReset, name);
      return name;
    }
    public Task Start(HostOptions options) {
      return Task.Run(delegate {
        if (Running) throw new InvalidOperationException("Host already running.");
        StopCore(); // clean up any helpers left by an unexpected worker exit
        if (options.Port < 1024 || options.Port > 65535) throw new ArgumentException("Port must be 1024–65535.");
        var reservation = new TcpListener(IPAddress.Any, options.Port);
        try { reservation.Server.ExclusiveAddressUse = true; reservation.Start(); }
        catch (SocketException) { throw new InvalidOperationException("TCP port " + options.Port + " is already in use. Stop the earlier host first."); }
        finally { reservation.Stop(); }
        Port = options.Port;
        try {
          var args = new List<string> {"--listen", Port.ToString(CultureInfo.InvariantCulture),
            "--fps", options.Fps.ToString(), "--bitrate", options.Bitrate.ToString(),
            "--codec", options.Encoder, "--stop-event", NewStopEvent(out mediaStop)};
          if (!String.IsNullOrEmpty(options.Display)) args.AddRange(new [] {"--monitor", options.Display});
          if (options.Synthetic) args.Add("--synthetic");
          if (options.ScreenOff) args.Add("--screen-off");
          if (options.NoAudio) args.Add("--no-audio");
          if (options.AllowInput) {
            byte[] random = new byte[8]; using (var rng = RandomNumberGenerator.Create()) rng.GetBytes(random);
            token = BitConverter.ToUInt64(random, 0);
            pipeName = "wvd.input.ui." + Process.GetCurrentProcess().Id + "." + Guid.NewGuid().ToString("N");
            string auth = token.ToString("X16", CultureInfo.InvariantCulture);
            input = Spawn("input/wvd-input.exe", new [] {"--pipe", "\\\\.\\pipe\\" + pipeName,
              "--token", auth, "--stop-event", NewStopEvent(out inputStop)}, false);
            args.AddRange(new [] {"--input", "\\\\.\\pipe\\" + pipeName, "--input-token", auth});
          }
          media = Spawn("media/wvd-media.exe", args, true);
          Thread.Sleep(600);
          if (!Running || (options.AllowInput && !Alive(input)))
            throw new InvalidOperationException("A host component exited. See the activity log.");
          Emit("UI_HOST_STARTED");
        } catch { StopCore(); throw; }
      });
    }
    private static void End(ref Process process, ref EventWaitHandle signal) {
      if (signal != null) signal.Set();
      if (process != null) {
        try { if (!process.HasExited && !process.WaitForExit(signal != null ? 3500 : 0)) process.Kill();
          process.WaitForExit(2000); } catch { }
        process.Dispose(); process = null;
      }
      if (signal != null) { signal.Dispose(); signal = null; }
    }
    private void StopCore() {
      EventWaitHandle unused = null;
      End(ref receiver, ref unused);
      End(ref media, ref mediaStop); // normal shutdown flushes the armed connection
      End(ref input, ref inputStop);
      token = 0; pipeName = null;
    }
    public Task Stop() { return Task.Run(delegate { StopCore(); Emit("UI_HOST_STOPPED"); }); }
    public Task ToggleReceiver() {
      return Task.Run(delegate {
        if (ReceiverRunning) { EventWaitHandle unused = null; End(ref receiver, ref unused); Emit("UI_TEST_STOPPED"); }
        else {
          if (!Running) throw new InvalidOperationException("Start the host before running a local test.");
          receiver = Spawn("tools/diag/wvd-diag.exe", new [] {"sink", "--port", Port.ToString()}, true);
          Emit("UI_TEST_STARTED");
        }
      });
    }
    public void ToggleCard() {
      if (Alive(card)) { card.CloseMainWindow(); return; }
      card = Spawn("tools/testcard/wvd-testcard.exe", new [] {"--big"}, false);
      Emit("Test card opened: an animated reference pattern and frame counter. Press Esc to close it.");
    }
    public Task DisableInput() {
      return Task.Run(delegate {
        if (!InputAvailable || String.IsNullOrEmpty(pipeName)) return;
        using (var stream = new NamedPipeClientStream(".", pipeName, PipeDirection.InOut, PipeOptions.Asynchronous)) {
          stream.Connect(500);
          byte[] packet;
          using (var bytes = new MemoryStream()) using (var writer = new BinaryWriter(bytes)) {
            writer.Write(0x50445657u); writer.Write((ushort)1); writer.Write((ushort)32);
            writer.Write(16u); writer.Write(0ul); writer.Write(21u); writer.Write(0u); writer.Write(token);
            packet = bytes.ToArray();
          }
          stream.Write(packet, 0, packet.Length); stream.Flush();
          var response = new byte[52]; int offset = 0;
          while (offset < response.Length) {
            var read = stream.ReadAsync(response, offset, response.Length - offset);
            if (!read.Wait(1500) || read.Result == 0) throw new IOException("Input helper did not confirm disarm.");
            offset += read.Result;
          }
          if (BitConverter.ToUInt32(response, 20) != 0) throw new IOException("Input helper is still armed.");
        }
        Emit("UI_INPUT_DISABLED");
      });
    }
    public Task ToggleDirectNetwork(string ssid, string password) {
      return Task.Run(delegate {
        if (NetworkRunning) { End(ref network, ref networkStop); return; }
        if (String.IsNullOrWhiteSpace(ssid) || Encoding.UTF8.GetByteCount(ssid) > 32)
          throw new ArgumentException("Use a network name of 1–32 UTF-8 bytes.");
        if (password.Length < 8 || password.Length > 63 || password.Any(c => c < 32 || c > 126))
          throw new ArgumentException("Use a Wi-Fi password of 8–63 printable ASCII characters.");
        network = Spawn("network/wvd-network.exe", new [] {"--ssid", ssid,
          "--stop-event", NewStopEvent(out networkStop)}, true);
        network.StandardInput.WriteLine(password); network.StandardInput.Close();
        Emit("Creating direct Wi-Fi. No internet connection is required.");
      });
    }
    public Task<string> Probe(string component, string argument) {
      return Task.Run(delegate {
        using (var process = new Process()) {
          process.StartInfo = new ProcessStartInfo(Exe(component), argument) {
            UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true,
            RedirectStandardError = true, StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8
          };
          process.Start();
          if (!AssignProcessToJobObject(job, process.Handle) && !process.HasExited) { process.Kill(); throw new IOException("Cannot contain probe."); }
          var stdout = process.StandardOutput.ReadToEndAsync();
          var stderr = process.StandardError.ReadToEndAsync();
          if (!process.WaitForExit(10000)) { process.Kill(); throw new TimeoutException("Device probe timed out."); }
          Task.WaitAll(stdout, stderr);
          return stdout.Result + stderr.Result;
        }
      });
    }
    public static string LocalAddress(int port, bool directOnly = false) {
      try {
        var adapters = NetworkInterface.GetAllNetworkInterfaces().Where(n => n.OperationalStatus == OperationalStatus.Up &&
          n.NetworkInterfaceType != NetworkInterfaceType.Loopback && n.NetworkInterfaceType != NetworkInterfaceType.Tunnel);
        if (directOnly) adapters = adapters.Where(n => n.Description.IndexOf("Wi-Fi Direct", StringComparison.OrdinalIgnoreCase) >= 0);
        var address = adapters.SelectMany(n => n.GetIPProperties().UnicastAddresses)
          .Select(a => a.Address).FirstOrDefault(a => a.AddressFamily == AddressFamily.InterNetwork &&
          !IPAddress.IsLoopback(a) && (directOnly || !a.ToString().StartsWith("169.254.")));
        return address == null && directOnly ? "Waiting for direct Wi-Fi address…" :
          (address == null ? "127.0.0.1" : address.ToString()) + ":" + port;
      } catch { return directOnly ? "Waiting for direct Wi-Fi address…" : "127.0.0.1:" + port; }
    }
    public void Dispose() {
      if (disposed) return; disposed = true;
      StopCore(); End(ref network, ref networkStop);
      EventWaitHandle unused = null; End(ref card, ref unused);
      if (job != IntPtr.Zero) CloseHandle(job);
    }
    [StructLayout(LayoutKind.Sequential)] private struct BasicLimits {
      public long ProcessTime, JobTime; public uint LimitFlags;
      public UIntPtr MinimumWorkingSetSize, MaximumWorkingSetSize;
      public uint ActiveProcessLimit; public UIntPtr Affinity; public uint PriorityClass, SchedulingClass;
    }
    [StructLayout(LayoutKind.Sequential)] private struct IoCounters { public ulong A,B,C,D,E,F; }
    [StructLayout(LayoutKind.Sequential)] private struct JobLimits {
      public BasicLimits BasicLimitInformation; public IoCounters IoInfo;
      public UIntPtr ProcessMemoryLimit, JobMemoryLimit, PeakProcessMemoryUsed, PeakJobMemoryUsed;
    }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] private static extern IntPtr CreateJobObject(IntPtr attributes, string name);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool SetInformationJobObject(IntPtr job, int info, IntPtr value, uint size);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
    [DllImport("kernel32.dll")] private static extern bool CloseHandle(IntPtr handle);
  }
}
