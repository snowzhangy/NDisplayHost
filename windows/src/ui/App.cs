using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Reflection;
using System.Security.Cryptography;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Markup;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Shapes;
using System.Windows.Threading;

namespace WvdHost {
  public static class App {
    [STAThread] public static int Main(string[] args) {
      try {
        using (var controller = new HostController()) {
          var application = new Application { ShutdownMode = ShutdownMode.OnMainWindowClose };
          using (var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream("WvdHost.MainWindow.xaml")) {
            var window = (Window)XamlReader.Load(stream);
            var panel = new ControlPanel(window, controller, args);
            application.Run(window);
            GC.KeepAlive(panel);
          }
        }
        return 0;
      } catch (Exception error) {
        MessageBox.Show(error.Message, HostInfo.Name + " could not open", MessageBoxButton.OK,
          MessageBoxImage.Error);
        return 1;
      }
    }
  }
  public sealed class Metrics {
    public string Fps, Mbps, EncodeMs, Encoder, Pacing;
    public static Metrics Parse(string line) {
      var rate = Regex.Match(line, @"(?<kbps>\d+)kbps (?<fps>\d+)fps.*enc=(?<encoder>[\w-]+)");
      var timing = Regex.Match(line, @"enc p50=(?<ms>[\d.]+)ms");
      if (!rate.Success || !timing.Success) return null;
      double kbps;
      if (!Double.TryParse(rate.Groups["kbps"].Value, NumberStyles.Float, CultureInfo.InvariantCulture, out kbps)) return null;
      var pacing = Regex.Match(line, @"gaps\[cap/send\]=([\d.]+)/([\d.]+)ms send-p95=([\d.]+)ms");
      return new Metrics { Fps = rate.Groups["fps"].Value, Mbps = (kbps / 1000).ToString("0.0", CultureInfo.InvariantCulture),
        EncodeMs = timing.Groups["ms"].Value, Encoder = rate.Groups["encoder"].Value,
        Pacing = pacing.Success ? "Worst gap: capture " + pacing.Groups[1].Value + " ms · send " + pacing.Groups[2].Value +
          " ms. Send p95: " + pacing.Groups[3].Value + " ms." : null };
    }
  }
  public sealed class ControlPanel {
    readonly Window window;
    readonly HostController controller;
    readonly List<string> displays = new List<string>();
    readonly List<string> activity = new List<string>();
    readonly DispatcherTimer timer = new DispatcherTimer();
    readonly string[] arguments;
    readonly TrayIcon tray;
    WindowState restoreState = WindowState.Normal;
    Window logWindow;
    TextBox logText;
    bool ready, busy, streaming, inputDisabled, allowClose, closing, lastRunning, initialized;
    string directAddress;
    T Find<T>(string name) where T : class { return window.FindName(name) as T; }
    void Text(string name, string value) { Find<TextBlock>(name).Text = value; }
    void Footer(string value) { Text("FooterText", value); }
    public ControlPanel(Window window, HostController controller, string[] arguments) {
      this.window = window; this.controller = controller; this.arguments = arguments;
      // The version travels with every surface the user can see, so a tray
      // tooltip or a screenshot is enough to say which host build is running.
      window.Title = HostInfo.Full;
      Text("ProductTitle", HostInfo.Product);
      activity.Add(HostInfo.Full + " control panel");
      byte[] random = new byte[12]; using (var rng = RandomNumberGenerator.Create()) rng.GetBytes(random);
      Find<PasswordBox>("WifiPassword").Password = Convert.ToBase64String(random);
      string shortName = Environment.MachineName;
      if (shortName.Length > 16) shortName = shortName.Substring(0, 16);
      Find<TextBox>("SsidText").Text = "WVM-" + shortName;
      controller.Line += delegate(string line) {
        if (!window.Dispatcher.HasShutdownStarted) window.Dispatcher.BeginInvoke(new Action(delegate { OnLine(line); }));
      };
      Find<Button>("StartButton").Click += async delegate { await Run(StartHost); };
      Find<Button>("RefreshDisplaysButton").Click += async delegate { await Run(delegate { return RefreshDisplays(false); }); };
      Find<Button>("StopButton").Click += async delegate { await Run(controller.Stop); };
      Find<Button>("ReceiverButton").Click += async delegate { await Run(controller.ProbeListener); };
      Find<Button>("DisableInputButton").Click += async delegate { await Run(controller.DisableInput); };
      Find<Button>("TestCardButton").Click += delegate {
        try { controller.ToggleCard(); } catch (Exception error) { Footer(error.Message); }
      };
      Find<Button>("DirectButton").Click += async delegate {
        string ssid = Find<TextBox>("SsidText").Text, password = Find<PasswordBox>("WifiPassword").Password;
        await Run(delegate { return controller.ToggleDirectNetwork(ssid, password); });
        if (!controller.NetworkRunning) directAddress = null;
      };
      Find<Button>("CopyAddressButton").Click += delegate { Copy(Find<TextBox>("AddressText").Text, "Receiver address copied."); };
      Find<Button>("CopyPasswordButton").Click += delegate { Copy(Find<PasswordBox>("WifiPassword").Password, "Wi-Fi password copied. Join this network in Vision Pro Wi-Fi settings."); };
      Find<Button>("LogsButton").Click += delegate { ShowActivity(); };
      timer.Interval = TimeSpan.FromMilliseconds(500);
      timer.Tick += delegate { Refresh(); };
      window.Loaded += async delegate {
        if (initialized) return;
        initialized = true;
        timer.Start(); await Probe();
        if (arguments.Length == 2 && arguments[0] == "--smoke-test") await SmokeTest(arguments[1]);
        if (arguments.Length == 2 && arguments[0] == "--snapshot") Snapshot(arguments[1]);
      };
      window.Closing += OnClosing;
      tray = new TrayIcon(RestoreWindow, delegate { window.Close(); });
      window.Icon = tray.WindowIcon;
      window.StateChanged += delegate {
        if (window.WindowState != WindowState.Minimized) { restoreState = window.WindowState; return; }
        if (closing || !tray.Visible) return;
        if (logWindow != null) logWindow.Hide();
        window.ShowInTaskbar = false;
        window.Hide();
      };
      window.Closed += delegate { tray.Dispose(); };
      Refresh();
    }
    void RestoreWindow() {
      if (closing) return;
      window.ShowInTaskbar = true;
      window.WindowState = restoreState;
      window.Show();
      window.Activate();
    }
    void Copy(string value, string message) {
      try { Clipboard.SetText(value); Footer(message); } catch { Footer("Clipboard is busy. Please try again."); }
    }
    HostOptions Options() {
      var display = Find<ComboBox>("DisplayChoice").SelectedIndex;
      var encoder = Find<ComboBox>("EncoderChoice").SelectedItem as ComboBoxItem;
      return new HostOptions { Display = display >= 0 && display < displays.Count ? displays[display] : "",
        Encoder = encoder == null ? "auto" : (string)encoder.Tag,
        Fps = new [] {30,60,90}[Find<ComboBox>("FpsChoice").SelectedIndex],
        Bitrate = new [] {20000,40000,60000,80000}[Find<ComboBox>("BitrateChoice").SelectedIndex],
        AllowInput = Find<CheckBox>("AllowInput").IsChecked == true,
        ScreenOff = Find<CheckBox>("ScreenOff").IsChecked == true,
        NoAudio = Find<CheckBox>("SendAudio").IsChecked != true };
    }
    async Task Run(Func<Task> action) {
      if (busy) return;
      busy = true; Refresh();
      try { await action(); }
      catch (Exception error) { Footer(error.Message); OnLine("ERROR: " + error.Message); }
      finally { busy = false; Refresh(); }
    }
    async Task StartHost() {
      await RefreshDisplays(true);
      await controller.Start(Options());
    }
    bool PopulateDisplays(string adapters, string selected) {
      var choices = Find<ComboBox>("DisplayChoice");
      displays.Clear(); choices.Items.Clear();
      displays.Add(""); choices.Items.Add("Primary display (automatic)");
      int selectedIndex = String.IsNullOrEmpty(selected) ? 0 : -1;
      foreach (Match display in Regex.Matches(adapters, @"(?m)^\s*(\\\\\.\\DISPLAY\d+)\s+rect=\((-?\d+),(-?\d+)\s+(\d+)x(\d+)\)[^\r\n]*")) {
        var identity = Regex.Match(display.Value, @"devicePath=""([^""]+)""");
        string name = display.Groups[1].Value;
        string selector = identity.Success ? identity.Groups[1].Value : name;
        if (String.Equals(selector, selected, StringComparison.OrdinalIgnoreCase) ||
            String.Equals(name, selected, StringComparison.OrdinalIgnoreCase)) selectedIndex = displays.Count;
        displays.Add(selector);
        choices.Items.Add(name.Replace(@"\\.\", "") + "   ·   " +
          display.Groups[4].Value + " × " + display.Groups[5].Value);
      }
      choices.SelectedIndex = selectedIndex >= 0 ? selectedIndex : 0;
      return selectedIndex >= 0;
    }
    async Task RefreshDisplays(bool requireSelection) {
      int index = Find<ComboBox>("DisplayChoice").SelectedIndex;
      string selected = index >= 0 && index < displays.Count ? displays[index] : "";
      string adapters = await controller.Probe("tools/diag/wvd-diag.exe", "adapters");
      bool found = PopulateDisplays(adapters, selected);
      if (!found) {
        const string message = "The selected display is no longer available. Choose an active display, then start again.";
        if (requireSelection) throw new InvalidOperationException(message);
        Footer(message);
      } else if (!requireSelection) Footer("Display list refreshed.");
    }
    async Task Probe() {
      try {
        string adapters = await controller.Probe("tools/diag/wvd-diag.exe", "adapters");
        var gpu = Regex.Match(adapters, @"(?m)^\s*\[0\]\s+(.+?)\s+luid=");
        PopulateDisplays(adapters, "");
        var encoders = Find<ComboBox>("EncoderChoice");
        encoders.Items.Add(new ComboBoxItem { Content = "Automatic · best available", Tag = "auto" });
        string caps = await controller.Probe("tools/diag/wvd-diag.exe", "caps");
        foreach (string name in new [] {"mf-hw", "nvenc-hevc", "mf-sw-h264"})
          if (Regex.IsMatch(caps, @"(?m)^\s*" + name + @"\s+.*\bAVAILABLE\b"))
            encoders.Items.Add(new ComboBoxItem { Content = name == "mf-hw" ? "GPU hardware · Media Foundation" :
              name == "nvenc-hevc" ? "NVIDIA NVENC · HEVC" : "Software · H.264 (slower)", Tag = name });
        encoders.SelectedIndex = 0;
        Text("GpuLabel", (gpu.Success ? gpu.Groups[1].Value.Trim() : "Windows graphics adapter") +
          (caps.Contains("mf-hw") && Regex.IsMatch(caps, @"mf-hw[^\r\n]*AVAILABLE") ? " · hardware encoder available" : ""));
        string wifi = await controller.Probe("network/wvd-network.exe", "--probe");
        if (wifi.Contains("WIFI_ADAPTER")) Text("NetworkStatus", "Wi-Fi adapter found. Start checks direct-network support; the radio is unchanged until then.");
        else Text("NetworkStatus", "No Wi-Fi adapter found. Direct Wi-Fi requires a compatible wireless adapter.");
        ready = true; Footer("Ready. Start the host, then connect from Vision Pro.");
      } catch (Exception error) { Footer("Device check failed: " + error.Message); }
      Refresh();
    }
    void OnLine(string line) {
      if (closing) return;
      // Cursor-change traces are high frequency and don't help the activity view.
      if (line.Contains("cursor shape")) return;
      activity.Add(line); if (activity.Count > 180) activity.RemoveAt(0);
      if (logText != null && logWindow.IsVisible) { logText.Text = String.Join(Environment.NewLine, activity); logText.ScrollToEnd(); }
      var metrics = Metrics.Parse(line);
      if (metrics != null) {
        Text("FpsValue", metrics.Fps); Text("BitrateValue", metrics.Mbps); Text("EncodeValue", metrics.EncodeMs);
        Text("StreamDetail", metrics.Pacing ?? ("Desktop → " + metrics.Encoder + " → connected receiver. Values reflect screen changes."));
      }
      if (line.Contains("media session started:")) { streaming = true; Footer("Receiver connected. Desktop capture and video encoding are running."); }
      if (line.Contains("peer disconnected") || line.Contains("session ended")) { streaming = false; Text("StreamDetail", "Receiver disconnected. Waiting for another connection."); }
      if (line == "UI_LISTENER_TEST_OK") Footer("The local UDP listener completed a QUIC handshake.");
      var pairing = System.Text.RegularExpressions.Regex.Match(line, @"^PAIR (\d+) (\d{6}) (.*)$");
      ulong requestId;
      if (pairing.Success && !closing && UInt64.TryParse(pairing.Groups[1].Value, out requestId))
        ConfirmPairing(requestId, pairing.Groups[2].Value, pairing.Groups[3].Value);
      if (line == "UI_HOST_STARTED") { inputDisabled = false; Footer("Host listening. Connect from Vision Pro using the address above."); }
      if (line == "UI_HOST_STOPPED") {
        streaming = false; Text("StreamDetail", "Host stopped. Display capture and input have been released.");
        Text("FpsValue", "—"); Text("BitrateValue", "—"); Text("EncodeValue", "—"); Footer("Host stopped.");
      }
      if (line == "UI_INPUT_DISABLED") { inputDisabled = true; Footer("Keyboard and mouse control disabled. Stop and restart the host to allow it again."); }
      if (line == "DIRECT_WIFI_STARTED") { directAddress = null; Text("NetworkStatus", "Direct Wi-Fi is on. Join the network above in Vision Pro Settings → Wi-Fi. No internet is needed."); Footer("Direct Wi-Fi network created."); }
      if (line.StartsWith("DIRECT_WIFI_ADDRESS=")) directAddress = line.Substring("DIRECT_WIFI_ADDRESS=".Length);
      if (line.Contains("DIRECT_WIFI_FAILED")) { Text("NetworkStatus", "Windows could not start direct Wi-Fi. Check Wi-Fi is on and Windows Mobile Hotspot is off. See Activity log for the error."); Footer("Direct Wi-Fi could not start on this adapter/configuration."); }
      if (line == "DIRECT_WIFI_STOPPED") { directAddress = null; Text("NetworkStatus", "Direct Wi-Fi is off."); }
      if (line.Contains("session start failed")) { streaming = false; Footer("Capture could not start. See Activity log for details."); }
      Refresh();
    }
    async void ConfirmPairing(ulong requestId, string code, string deviceName) {
      if (!controller.Running) return;
      RestoreWindow();
      bool accepted = MessageBox.Show(window,
          "Pair with " + deviceName + " only if the headset shows the same code.\n\n" + code,
          "Confirm headset pairing", MessageBoxButton.YesNo, MessageBoxImage.Question,
          MessageBoxResult.No) == MessageBoxResult.Yes;
      try { await controller.DecidePairing(requestId, accepted); }
      catch (Exception error) { Footer("Pairing decision could not be sent. " + error.Message); }
    }

    void Refresh() {
      bool running = controller.Running, direct = controller.NetworkRunning;
      if (lastRunning && !running && !busy && !closing) Footer("The host exited. See Activity log, then start it again.");
      lastRunning = running;
      foreach (string name in new [] {"DisplayChoice","FpsChoice","BitrateChoice","EncoderChoice"}) Find<ComboBox>(name).IsEnabled = ready && !running && !busy;
      foreach (string name in new [] {"AllowInput","SendAudio","ScreenOff"}) Find<CheckBox>(name).IsEnabled = ready && !running && !busy;
      Find<Button>("StartButton").IsEnabled = ready && !running && !busy;
      Find<Button>("RefreshDisplaysButton").IsEnabled = ready && !running && !busy;
      Find<Button>("StopButton").IsEnabled = (running || controller.InputAvailable) && !busy;
      Find<Button>("ReceiverButton").IsEnabled = running && !busy;
      Find<Button>("DisableInputButton").IsEnabled = running && controller.InputAvailable && !inputDisabled && !busy;
      Find<Button>("DisableInputButton").Visibility = controller.InputAvailable ? Visibility.Visible : Visibility.Collapsed;
      Find<Button>("DirectButton").IsEnabled = ready && !busy;
      Find<Button>("DirectButton").Content = direct ? "Stop direct Wi-Fi" : "Create direct Wi-Fi";
      Find<TextBox>("SsidText").IsEnabled = !direct && !busy;
      Find<PasswordBox>("WifiPassword").IsEnabled = !direct && !busy;
      string status = busy ? "Working…" : running ? (streaming ? "Streaming" : "Waiting for receiver") : "Host stopped";
      Text("StatusBadge", status);
      tray.SetStatus(status);
      Find<Ellipse>("StatusDot").Fill = new SolidColorBrush(running ? Color.FromRgb(91, 218, 166) : Color.FromRgb(153,166,186));
      string address = direct && directAddress != null ? directAddress + ":" + controller.Port : HostController.LocalAddress(controller.Port, direct);
      Find<TextBox>("AddressText").Text = address;
      Find<Button>("CopyAddressButton").IsEnabled = !address.StartsWith("Waiting");
    }
    void ShowActivity() {
      if (logWindow != null) { logWindow.Show(); logWindow.Activate(); return; }
      logText = new TextBox { IsReadOnly = true, Text = String.Join(Environment.NewLine, activity),
        FontFamily = new FontFamily("Consolas"), FontSize = 12, TextWrapping = TextWrapping.NoWrap,
        Background = new SolidColorBrush(Color.FromRgb(16,20,30)), Foreground = Brushes.White,
        BorderThickness = new Thickness(0), Padding = new Thickness(18),
        VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Auto };
      logWindow = new Window { Title = HostInfo.Product + " · Activity", Width = 920, Height = 450, Content = logText,
        Owner = window, WindowStartupLocation = WindowStartupLocation.CenterOwner };
      logWindow.Closing += delegate(object sender, CancelEventArgs e) { if (!allowClose) { e.Cancel = true; logWindow.Hide(); } };
      logWindow.Show();
    }
    async void OnClosing(object sender, CancelEventArgs e) {
      if (allowClose) return;
      e.Cancel = true;
      if (closing) return;
      closing = true; busy = true; timer.Stop(); Footer("Stopping capture, input and direct Wi-Fi…"); Refresh();
      await Task.Run(delegate { controller.Dispose(); });
      allowClose = true; window.Close();
    }
    async Task SmokeTest(string output) {
      try {
        if (!ready || displays.Count == 0) throw new Exception("Device choices did not initialize.");
        if (displays.Count > 1) {
          string selected = displays[1];
          Find<ComboBox>("DisplayChoice").SelectedIndex = 1;
          await RefreshDisplays(true);
          if (Options().Display != selected) throw new Exception("Refreshing changed the selected monitor identity.");
        }
        Find<ComboBox>("DisplayChoice").SelectedIndex = 0;
        await RefreshDisplays(true);
        if (Options().Display != "") throw new Exception("Automatic display selection became a fixed GDI name.");
        int port;
        using (var reservation = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0)))
          port = ((IPEndPoint)reservation.Client.LocalEndPoint).Port;
        var options = Options(); options.Port = port; options.AllowInput = true; options.Synthetic = true;
        await controller.Start(options);
        await controller.ProbeListener();
        window.WindowState = WindowState.Minimized;
        await Task.Delay(800);
        if (window.IsVisible || window.ShowInTaskbar || !tray.Visible || !controller.Running)
          throw new Exception("Minimize-to-tray interrupted the listener or left the panel visible.");
        await controller.ProbeListener();
        RestoreWindow();
        if (!window.IsVisible || !window.ShowInTaskbar || window.WindowState != WindowState.Normal)
          throw new Exception("The tray could not restore the control panel.");
        await controller.DisableInput();
        await controller.Stop();
        if (controller.Running || controller.InputAvailable) throw new Exception("Owned processes did not stop.");
        File.WriteAllText(output, "PASS: UI display refresh/identity selection, QUIC listener probe, minimize-to-tray with uninterrupted listening, restore, input disarm and graceful process cleanup.");
      } catch (Exception error) { File.WriteAllText(output, "FAIL: " + error); }
      window.Close();
    }
    void Snapshot(string output) {
      // Render this WPF visual directly; never capture other desktop windows.
      window.UpdateLayout();
      var bitmap = new RenderTargetBitmap((int)window.ActualWidth, (int)window.ActualHeight, 96, 96, PixelFormats.Pbgra32);
      bitmap.Render(window);
      var encoder = new PngBitmapEncoder(); encoder.Frames.Add(BitmapFrame.Create(bitmap));
      using (var stream = File.Create(output)) encoder.Save(stream);
      window.Close();
    }
  }
}
