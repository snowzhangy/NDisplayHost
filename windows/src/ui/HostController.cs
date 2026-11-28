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
    public string StereoLayout = "none";
    public string StereoSize = "";
    public int Fps = 60, Bitrate = 60000, Port = 47800;
    public bool AllowInput, Synthetic, ScreenOff, NoAudio, GameDepth;
  }

  // Direct-connect network name, password and band, kept between launches so
  // Vision Pro rejoins a network it already knows. The password is encrypted
  // for this Windows user (DPAPI) in the same store as pairings (%WVM_HOME% or
  // %LOCALAPPDATA%\WVM).
  public sealed class DirectSettings {
    public string Ssid, Password, Band = "5";
    // Was direct connect on when the panel last closed? Then it comes back on.
    public bool Enabled;
    static string FilePath {
      get {
        string home = Environment.GetEnvironmentVariable("WVM_HOME");
        if (String.IsNullOrEmpty(home))
          home = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "WVM");
        return Path.Combine(home, "direct-connect.txt");
      }
    }
    public static string NewPassword() {
      // Typed on the visionOS keyboard while reading it off the PC screen:
      // lowercase only, no look-alikes (0/o, 1/l/i), three groups of four.
      // 31 symbols x 12 ~ 59 bits; bytes >= 248 are redrawn to avoid modulo bias.
      const string alphabet = "abcdefghjkmnpqrstuvwxyz23456789";
      var text = new StringBuilder();
      var one = new byte[1];
      using (var rng = RandomNumberGenerator.Create()) {
        for (int count = 0; count < 12;) {
          rng.GetBytes(one);
          if (one[0] >= 248) continue;
          if (count > 0 && count % 4 == 0) text.Append('-');
          text.Append(alphabet[one[0] % alphabet.Length]);
          ++count;
        }
      }
      return text.ToString();
    }
    public static DirectSettings Load(string defaultSsid) {
      var settings = new DirectSettings { Ssid = defaultSsid };
      try {
        foreach (string line in File.ReadAllLines(FilePath)) {
          int split = line.IndexOf('=');
          if (split < 0) continue;
          string key = line.Substring(0, split), value = line.Substring(split + 1);
          if (key == "ssid" && value.Length > 0) settings.Ssid = value;
          if (key == "band" && (value == "5" || value == "auto" || value == "2.4")) settings.Band = value;
          if (key == "enabled") settings.Enabled = value == "1";
          if (key == "password") settings.Password = Encoding.UTF8.GetString(ProtectedData.Unprotect(
            Convert.FromBase64String(value), null, DataProtectionScope.CurrentUser));
        }
      } catch { }  // missing or unreadable: start fresh
      if (String.IsNullOrWhiteSpace(settings.Ssid) || Encoding.UTF8.GetByteCount(settings.Ssid) > 32)
        settings.Ssid = defaultSsid;
      try { HostController.ValidateDirect(settings.Ssid, settings.Password ?? ""); }
      catch (ArgumentException) { settings.Password = NewPassword(); }
      return settings;
    }
    public void Save() {
      string path = FilePath;
      Directory.CreateDirectory(Path.GetDirectoryName(path));
      string secret = Convert.ToBase64String(ProtectedData.Protect(Encoding.UTF8.GetBytes(Password), null,
        DataProtectionScope.CurrentUser));
      File.WriteAllLines(path, new [] {"ssid=" + Ssid, "band=" + Band, "enabled=" + (Enabled ? "1" : "0"), "password=" + secret});
    }
  }

  // Every role runs as a child process of this UI's kill-on-close job, started
  // from the single command-line host: ndisplay.exe <role>. Normal stops use an
  // event: the stream role releases/disarms input before the helper shuts down.
  public sealed class HostController : IDisposable {
    public event Action<string> Line;
    public readonly string Root;
    // The command-line host lives next to the panel, in the same folder.
    public const string CliName = "ndisplay.exe";
    private readonly IntPtr job;
    private Process media, input, network;
    private EventWaitHandle mediaStop, inputStop, networkStop;
    private string pipeName;
    private ulong token;
    private bool disposed;
    public int Port { get; private set; }
    public bool Running { get { return Alive(media); } }
    public bool NetworkRunning { get { return Alive(network); } }
    public bool InputAvailable { get { return Alive(input); } }

    public HostController() {
      Root = AppDomain.CurrentDomain.BaseDirectory;
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
      Port = 47800;
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
    private string Cli { get { return Path.Combine(Root, CliName); } }
    // args begin with the role name: stream, input, network, diag.
    private Process Spawn(IEnumerable<string> args, bool captureOutput,
                          Action<string> onOutput = null) {
      string executable = Cli;
      if (!File.Exists(executable))
        throw new FileNotFoundException(CliName + " is missing next to " +
          Path.GetFileName(Process.GetCurrentProcess().MainModule.FileName));
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
        if (!captureOutput || String.IsNullOrEmpty(e.Data)) return;
        if (onOutput != null) onOutput(e.Data);
        Emit(e.Data);
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
        Port = options.Port;
        try {
          var args = new List<string> {"stream", "--listen", Port.ToString(CultureInfo.InvariantCulture),
            "--fps", options.Fps.ToString(), "--bitrate", options.Bitrate.ToString(),
            "--codec", options.Encoder, "--stop-event", NewStopEvent(out mediaStop)};
          if (!String.IsNullOrEmpty(options.Display)) args.AddRange(new [] {"--monitor", options.Display});
          if (options.Synthetic) args.Add("--synthetic");
          if (options.StereoLayout != "none") {
            args.AddRange(new [] {"--stereo-layout", options.StereoLayout});
            if (!String.IsNullOrEmpty(options.StereoSize))
              args.AddRange(new [] {"--stereo-size", options.StereoSize});
          } else if (options.GameDepth && !options.Synthetic)
            args.Add("--game-depth");
          if (options.ScreenOff) args.Add("--screen-off");
          if (options.NoAudio) args.Add("--no-audio");
          if (options.AllowInput) {
            byte[] random = new byte[8]; using (var rng = RandomNumberGenerator.Create()) rng.GetBytes(random);
            token = BitConverter.ToUInt64(random, 0);
            pipeName = "wvd.input.ui." + Process.GetCurrentProcess().Id + "." + Guid.NewGuid().ToString("N");
            string auth = token.ToString("X16", CultureInfo.InvariantCulture);
            input = Spawn(new [] {"input", "--pipe", "\\\\.\\pipe\\" + pipeName,
              "--token", auth, "--stop-event", NewStopEvent(out inputStop)}, false);
            args.AddRange(new [] {"--input", "\\\\.\\pipe\\" + pipeName, "--input-token", auth, "--allow-input"});
          }
          var listening = new TaskCompletionSource<string>();
          string readyLine = "HOST_READY " + Port.ToString(CultureInfo.InvariantCulture);
          const string listenError = "HOST_LISTEN_ERROR ";
          media = Spawn(args, true, line => {
            if (line == readyLine) listening.TrySetResult("");
            if (line.StartsWith(listenError, StringComparison.Ordinal))
              listening.TrySetResult(line.Substring(listenError.Length));
          });
          var timeout = Stopwatch.StartNew();
          while (!listening.Task.Wait(50) && timeout.ElapsedMilliseconds < 8000 && Running) { }
          if (!Running) media.WaitForExit(); // include the worker's final stdout
          if (listening.Task.IsCompleted && listening.Task.Result.Length != 0)
            throw new InvalidOperationException(listening.Task.Result);
          if (!listening.Task.IsCompleted || !Running || (options.AllowInput && !Alive(input)))
            throw new InvalidOperationException("Could not start the UDP listener. See the activity log.");
          Emit("UI_HOST_STARTED");
        } catch { StopCore(); throw; }
      });
    }
    private static void End(ref Process process, ref EventWaitHandle signal, int graceMs = 3500) {
      if (signal != null) signal.Set();
      if (process != null) {
        try { if (!process.HasExited && !process.WaitForExit(signal != null ? graceMs : 0)) process.Kill();
          process.WaitForExit(2000); } catch { }
        process.Dispose(); process = null;
      }
      if (signal != null) { signal.Dispose(); signal = null; }
    }
    private void StopCore() {
      End(ref media, ref mediaStop); // normal shutdown flushes the armed connection
      End(ref input, ref inputStop);
      token = 0; pipeName = null;
    }
    public Task Stop() { return Task.Run(delegate { StopCore(); Emit("UI_HOST_STOPPED"); }); }
    public Task AllowFirewall() {
      return Task.Run(delegate {
        var info = new ProcessStartInfo(Cli, "diag firewall --install") {
          WorkingDirectory = Root, UseShellExecute = true, Verb = "runas",
          WindowStyle = ProcessWindowStyle.Hidden
        };
        using (var process = Process.Start(info)) {
          if (process == null || !process.WaitForExit(15000) || process.ExitCode != 0)
            throw new IOException("Windows Firewall did not add the UDP 47800 rule.");
        }
      });
    }
    public async Task ProbeListener() {
      if (!Running) throw new InvalidOperationException("Start the host before testing the listener.");
      await Probe("diag quic-probe " + Port.ToString(CultureInfo.InvariantCulture));
      Emit("UI_LISTENER_TEST_OK");
    }

    public Task DecidePairing(ulong requestId, bool accept) {
      var current = media;
      return Task.Run(delegate {
        if (!Alive(current) || current != media) return;
        using (var pipe = new NamedPipeClientStream(".", "wvm-pairing-" + current.Id, PipeDirection.Out)) {
          pipe.Connect(1500);
          var decision = Encoding.ASCII.GetBytes(requestId.ToString(CultureInfo.InvariantCulture) +
              (accept ? " accept\n" : " deny\n"));
          pipe.Write(decision, 0, decision.Length);
        }
      });
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
    public static void ValidateDirect(string ssid, string password) {
      if (String.IsNullOrWhiteSpace(ssid) || Encoding.UTF8.GetByteCount(ssid) > 32)
        throw new ArgumentException("Use a network name of 1–32 UTF-8 bytes.");
      if (password.Length < 8 || password.Length > 63 || password.Any(c => c < 32 || c > 126))
        throw new ArgumentException("Use a Wi-Fi password of 8–63 printable ASCII characters.");
    }
    // Direct connect: the network role turns this PC into the headset's access
    // point (Mobile Hotspot, else a local-only Wi-Fi Direct network).
    public Task StartDirectNetwork(string ssid, string password, string band) {
      return Task.Run(delegate {
        if (NetworkRunning) return;
        ValidateDirect(ssid, password);
        network = Spawn(new [] {"network", "--ssid", ssid, "--band", band,
          "--stop-event", NewStopEvent(out networkStop)}, true);
        network.StandardInput.WriteLine(password); network.StandardInput.Close();
        Emit("Turning on direct connect (" + ssid + ").");
      });
    }
    // Turning a hotspot off and restoring the user's hotspot settings can take
    // several seconds; a kill would skip that restore.
    private const int NetworkGraceMs = 10000;
    public Task StopDirectNetwork() {
      return Task.Run(delegate {
        if (network == null) return;
        End(ref network, ref networkStop, NetworkGraceMs);
        Emit("DIRECT_WIFI_UI_STOPPED");
      });
    }
    // Runs a command line of the host executable and returns its output.
    public Task<string> Probe(string argument) {
      return Task.Run(delegate {
        using (var process = new Process()) {
          process.StartInfo = new ProcessStartInfo(Cli, argument) {
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
          if (process.ExitCode != 0 && argument.StartsWith("quic-probe ", StringComparison.Ordinal))
            throw new IOException((stdout.Result + stderr.Result).Trim());
          return stdout.Result + stderr.Result;
        }
      });
    }
    // The address ICS gives the PC hosting a hotspot (see wvd/direct_link.h).
    static IPAddress HotspotAddress() {
      try {
        var value = Microsoft.Win32.Registry.GetValue(
          @"HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\SharedAccess\Parameters", "ScopeAddress", null) as string;
        IPAddress parsed;
        if (value != null && IPAddress.TryParse(value, out parsed)) return parsed;
      } catch { }
      return IPAddress.Parse("192.168.137.1");
    }
    public static string LocalAddress(int port, bool directOnly = false) {
      try {
        var hotspot = HotspotAddress();
        var adapters = NetworkInterface.GetAllNetworkInterfaces().Where(n => n.OperationalStatus == OperationalStatus.Up &&
          n.NetworkInterfaceType != NetworkInterfaceType.Loopback && n.NetworkInterfaceType != NetworkInterfaceType.Tunnel);
        var addresses = adapters.SelectMany(n => n.GetIPProperties().UnicastAddresses.Select(a => new { n, a.Address }))
          .Where(x => x.Address.AddressFamily == AddressFamily.InterNetwork && !IPAddress.IsLoopback(x.Address)).ToList();
        IPAddress address;
        if (directOnly)  // the hotspot port may be named after the Wi-Fi card (MT7925) or "Wi-Fi Direct"
          address = addresses.Where(x => x.Address.Equals(hotspot)).Select(x => x.Address).FirstOrDefault() ??
            addresses.Where(x => x.n.Description.IndexOf("Wi-Fi Direct", StringComparison.OrdinalIgnoreCase) >= 0 &&
              !x.Address.ToString().StartsWith("169.254.")).Select(x => x.Address).FirstOrDefault();
        else
          address = addresses.Where(x => !x.Address.Equals(hotspot) && !x.Address.ToString().StartsWith("169.254."))
            .Select(x => x.Address).FirstOrDefault();
        return address == null && directOnly ? "Waiting for direct Wi-Fi address…" :
          (address == null ? "127.0.0.1" : address.ToString()) + ":" + port;
      } catch { return directOnly ? "Waiting for direct Wi-Fi address…" : "127.0.0.1:" + port; }
    }
    public void Dispose() {
      if (disposed) return; disposed = true;
      StopCore(); End(ref network, ref networkStop, NetworkGraceMs);
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
