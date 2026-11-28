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
    string lanAddress;
    bool firewallPrompted, firewallPresent, directFirewallPresent;
    // Direct connect: what the network helper reported for the running network.
    DirectSettings directSettings;
    bool wifiAvailable = true, directOn, lastDirect;
    string directMode, directBand, directInternet, directAdopted, directProbe, directFailure;
    int directClients = -1, stationMhz;
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
      string shortName = Environment.MachineName;
      if (shortName.Length > 16) shortName = shortName.Substring(0, 16);
      directSettings = DirectSettings.Load("NDisplay-" + shortName);
      Find<TextBox>("SsidText").Text = directSettings.Ssid;
      Find<PasswordBox>("WifiPassword").Password = directSettings.Password;
      foreach (ComboBoxItem item in Find<ComboBox>("BandChoice").Items)
        if ((string)item.Tag == directSettings.Band) item.IsSelected = true;
      controller.Line += delegate(string line) {
        if (!window.Dispatcher.HasShutdownStarted) window.Dispatcher.BeginInvoke(new Action(delegate { OnLine(line); }));
      };
      Find<Button>("StartButton").Click += async delegate { await Run(StartHost); };
      Find<Button>("RefreshDisplaysButton").Click += async delegate { await Run(delegate { return RefreshDisplays(false); }); };
      Find<ComboBox>("StereoChoice").SelectionChanged += delegate { UpdateStereoSizes(); Refresh(); };
      UpdateStereoSizes();
      Find<Button>("StopButton").Click += async delegate { await Run(controller.Stop); };
      Find<Button>("ReceiverButton").Click += async delegate { await Run(controller.ProbeListener); };
      Find<Button>("DisableInputButton").Click += async delegate { await Run(controller.DisableInput); };
      // The switch's own IsChecked flip is overwritten by Refresh from the
      // helper's real state, so a failed start never leaves it showing "on".
      Find<CheckBox>("DirectToggle").Click += async delegate {
        if (controller.NetworkRunning) await Run(StopDirect);
        else await Run(StartDirect);
      };
      Find<Button>("NewPasswordButton").Click += delegate {
        Find<PasswordBox>("WifiPassword").Password = DirectSettings.NewPassword();
        Footer("New Wi-Fi password generated. Vision Pro will ask for it the next time it joins.");
      };
      Find<Button>("CopyAddressButton").Click += delegate { Copy(Find<TextBox>("AddressText").Text, "Receiver address copied."); };
      Find<Button>("LogsButton").Click += delegate { ShowActivity(); };
      Find<Button>("FirewallButton").Click += async delegate {
        await Run(async delegate {
          await controller.AllowFirewall();
          await RefreshFirewall();
        });
      };
      timer.Interval = TimeSpan.FromMilliseconds(500);
      timer.Tick += delegate { Refresh(); };
      window.Loaded += async delegate {
        if (initialized) return;
        initialized = true;
        timer.Start(); await Probe();
        // It was on when the panel last closed: the user wants it, so bring it
        // (and the host) straight back. Never in automated smoke/snapshot runs.
        if (arguments.Length == 0 && ready && wifiAvailable && directSettings.Enabled) {
          Footer("Direct connect was on last time. Turning it back on…");
          await Run(StartDirect);
        }
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
    string StereoLayout {
      get {
        var item = Find<ComboBox>("StereoChoice").SelectedItem as ComboBoxItem;
        return item == null ? "none" : (string)item.Tag;
      }
    }
    void UpdateStereoSizes() {
      var choices = Find<ComboBox>("StereoSizeChoice");
      choices.Items.Clear();
      choices.Items.Add(new ComboBoxItem { Content = "Automatic", Tag = "" });
      bool fullSbs = StereoLayout == "sbs";
      if (fullSbs || StereoLayout == "sbs-half") {
        foreach (var size in new [] {"1280x720", "1920x1080", "2560x1440", "3840x2160"})
          choices.Items.Add(new ComboBoxItem { Content = size.Replace("x", " × ") +
            (fullSbs ? " · half side by side" : ""), Tag = size + (fullSbs ? ":half-sbs" : "") });
        if (fullSbs)
          foreach (var size in new [] {"2560x720", "3840x1080", "5120x1440", "7680x2160"})
            choices.Items.Add(new ComboBoxItem { Content = size.Replace("x", " × ") +
              " · full side by side", Tag = size });
      }
      choices.SelectedIndex = 0;
    }
    HostOptions Options() {
      var display = Find<ComboBox>("DisplayChoice").SelectedIndex;
      var encoder = Find<ComboBox>("EncoderChoice").SelectedItem as ComboBoxItem;
      var stereoSize = Find<ComboBox>("StereoSizeChoice").SelectedItem as ComboBoxItem;
      return new HostOptions { Display = display >= 0 && display < displays.Count ? displays[display] : "",
        Encoder = encoder == null ? "auto" : (string)encoder.Tag,
        StereoLayout = StereoLayout,
        StereoSize = stereoSize == null ? "" : (string)stereoSize.Tag,
        Fps = new [] {30,60,90}[Find<ComboBox>("FpsChoice").SelectedIndex],
        Bitrate = new [] {20000,40000,60000,80000}[Find<ComboBox>("BitrateChoice").SelectedIndex],
        AllowInput = Find<CheckBox>("AllowInput").IsChecked == true,
        ScreenOff = Find<CheckBox>("ScreenOff").IsChecked == true,
        GameDepth = Find<CheckBox>("GameDepth").IsChecked == true,
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
      lanAddress = null;
      await EnsureFirewall();
      await controller.Start(Options());
    }
    // One question, once: the elevated step installs both the LAN rule and the
    // direct-connect rule (the PC's own network counts as Public to Windows).
    async Task EnsureFirewall() {
      if (firewallPrompted) return;
      firewallPrompted = true;
      await RefreshFirewall();
      if (firewallPresent && directFirewallPresent) return;
      if (MessageBox.Show(window,
          "Let Vision Pro connect to this PC?\n\nWindows will ask for administrator approval once, to allow " +
          "UDP port 47800 on your local network and on direct connect.",
          "Allow Vision Pro through the firewall", MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes) return;
      try { await controller.AllowFirewall(); }
      catch (Exception error) { OnLine("Firewall setup: " + error.Message); }
      await RefreshFirewall();
    }
    string Band {
      get { var item = Find<ComboBox>("BandChoice").SelectedItem as ComboBoxItem; return item == null ? "5" : (string)item.Tag; }
    }
    async Task StartDirect() {
      string ssid = Find<TextBox>("SsidText").Text.Trim(), password = Find<PasswordBox>("WifiPassword").Password;
      HostController.ValidateDirect(ssid, password);
      directSettings.Ssid = ssid; directSettings.Password = password; directSettings.Band = Band;
      try { directSettings.Save(); } catch (Exception error) { OnLine("Direct connect settings were not saved: " + error.Message); }
      ResetDirect(); directFailure = null;
      await EnsureFirewall();
      await controller.StartDirectNetwork(ssid, password, Band);
      directSettings.Enabled = true;
      try { directSettings.Save(); } catch { }
      // Nobody turns direct connect on for anything but streaming: start the
      // host too, so the headset finds this PC as soon as it joins.
      if (!controller.Running) await StartHost();
    }
    async Task StopDirect() {
      await controller.StopDirectNetwork();
      directSettings.Enabled = false;
      try { directSettings.Save(); } catch { }
    }
    void ResetDirect() {
      directOn = false; directMode = directBand = directInternet = directAdopted = directAddress = null;
      directClients = -1;
    }
    async Task RefreshFirewall() {
      string status;
      try { status = await controller.Probe("diag firewall"); }
      catch (Exception) { status = ""; }
      firewallPresent = status.Contains("FIREWALL_RULE_PRESENT");
      directFirewallPresent = status.Contains("FIREWALL_DIRECT_RULE_PRESENT");
      string details = firewallPresent && directFirewallPresent
          ? "Windows Firewall allows the headset on trusted local networks and on direct connect."
        : firewallPresent ? "Windows Firewall allows the headset on trusted local networks. Direct connect needs one more rule: Allow through firewall."
        : "Allow UDP 47800 through Windows Firewall to accept headset connections.";
      if (status.Contains("FIREWALL_PUBLIC"))
        details = "Windows reports an active Public network. On a trusted LAN, change its profile to Private in Windows Network & Internet settings. " + details;
      Text("FirewallStatus", details);
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
      string adapters = await controller.Probe("diag adapters");
      bool found = PopulateDisplays(adapters, selected);
      if (!found) {
        const string message = "The selected display is no longer available. Choose an active display, then start again.";
        if (requireSelection) throw new InvalidOperationException(message);
        Footer(message);
      } else if (!requireSelection) Footer("Display list refreshed.");
    }
    async Task Probe() {
      try {
        string adapters = await controller.Probe("diag adapters");
        var gpu = Regex.Match(adapters, @"(?m)^\s*\[0\]\s+(.+?)\s+luid=");
        PopulateDisplays(adapters, "");
        var encoders = Find<ComboBox>("EncoderChoice");
        encoders.Items.Add(new ComboBoxItem { Content = "Automatic · best available", Tag = "auto" });
        string caps = await controller.Probe("diag caps");
        foreach (string name in new [] {"mf-hw", "nvenc-hevc", "mf-sw-h264"})
          if (Regex.IsMatch(caps, @"(?m)^\s*" + name + @"\s+.*\bAVAILABLE\b"))
            encoders.Items.Add(new ComboBoxItem { Content = name == "mf-hw" ? "GPU hardware · Media Foundation" :
              name == "nvenc-hevc" ? "NVIDIA NVENC · HEVC" : "Software · H.264 (slower)", Tag = name });
        encoders.SelectedIndex = 0;
        Text("GpuLabel", (gpu.Success ? gpu.Groups[1].Value.Trim() : "Windows graphics adapter") +
          (caps.Contains("mf-hw") && Regex.IsMatch(caps, @"mf-hw[^\r\n]*AVAILABLE") ? " · hardware encoder available" : ""));
        // Read-only: the radio is unchanged until direct connect is switched on.
        string wifi = await controller.Probe("network --probe");
        wifiAvailable = wifi.Contains("WIFI_ADAPTER ");
        var station = Regex.Match(wifi, @"DIRECT_WIFI_STATION_MHZ=(\d+)");
        stationMhz = station.Success ? Int32.Parse(station.Groups[1].Value, CultureInfo.InvariantCulture) : 0;
        var hotspot = Regex.Match(wifi, @"(?m)^DIRECT_WIFI_HOTSPOT_AVAILABLE internet=(\w+) source=(.*?)\r?$");
        var unavailable = Regex.Match(wifi, @"(?m)^DIRECT_WIFI_HOTSPOT_UNAVAILABLE (.*?)\r?$");
        directProbe = !wifiAvailable ? "This PC has no Wi-Fi adapter, so direct connect is unavailable."
          : wifi.Contains("DIRECT_WIFI_HOTSPOT_ON") ? "Windows Mobile Hotspot is already on; direct connect will use it."
          : hotspot.Success && hotspot.Groups[1].Value == "shared" ? "The headset keeps internet through this PC."
          : hotspot.Success ? "Local only: this PC has no internet to share with the headset."
          : "Local only: the headset has no internet while joined (Mobile Hotspot is unavailable: " +
            (unavailable.Success ? unavailable.Groups[1].Value : "unknown reason") + ").";
        if (wifiAvailable && stationMhz > 0 && stationMhz < 3000)
          directProbe += " This PC's Wi-Fi is on 2.4 GHz, which would slow the headset too.";
        await RefreshFirewall();
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
        lanAddress = null;
        streaming = false; Text("StreamDetail", "Host stopped. Display capture and input have been released.");
        Text("FpsValue", "—"); Text("BitrateValue", "—"); Text("EncodeValue", "—"); Footer("Host stopped.");
      }
      if (line == "UI_INPUT_DISABLED") { inputDisabled = true; Footer("Keyboard and mouse control disabled. Stop and restart the host to allow it again."); }
      OnDirectLine(line);
      if (line.StartsWith("HOST_LAN_ADDRESS ", StringComparison.Ordinal))
        lanAddress = line.Substring("HOST_LAN_ADDRESS ".Length);
      if (line.Contains("session start failed")) { streaming = false; Footer("Capture could not start. See Activity log for details."); }
      Refresh();
    }
    static string After(string line, string prefix) {
      return line.StartsWith(prefix, StringComparison.Ordinal) ? line.Substring(prefix.Length) : null;
    }
    // Lines from the network helper (see src/network/main.cpp) and the
    // stream role's HOST_DIRECT_ADDRESS (its Bonjour on the PC's own network).
    void OnDirectLine(string line) {
      string value;
      if ((value = After(line, "DIRECT_WIFI_MODE=")) != null) directMode = value;
      else if ((value = After(line, "DIRECT_WIFI_BAND=")) != null) directBand = value;
      else if ((value = After(line, "DIRECT_WIFI_INTERNET=")) != null) directInternet = value;
      else if ((value = After(line, "DIRECT_WIFI_ADOPTED=")) != null) directAdopted = value;
      else if ((value = After(line, "DIRECT_WIFI_ADDRESS=")) != null) directAddress = value;
      else if ((value = After(line, "HOST_DIRECT_ADDRESS ")) != null) { if (controller.NetworkRunning && value != "unavailable") directAddress = value; }
      else if ((value = After(line, "DIRECT_WIFI_STATION_MHZ=")) != null) Int32.TryParse(value, out stationMhz);
      else if ((value = After(line, "DIRECT_WIFI_CLIENTS=")) != null) {
        int count; if (!Int32.TryParse(value, out count)) return;
        if (count > directClients && directClients >= 0) Footer("A device joined the direct-connect network.");
        directClients = count;
      }
      else if ((value = After(line, "DIRECT_WIFI_NOTE ")) != null) Footer(value);
      else if (line == "DIRECT_WIFI_STARTED") {
        directOn = true; directFailure = null;
        Footer("Direct connect is on. On Vision Pro, open Settings → Wi-Fi and join “" + DirectSsid + "”.");
      }
      else if (line == "DIRECT_WIFI_STOPPED" || line == "DIRECT_WIFI_UI_STOPPED") {
        if (directOn) Footer("Direct connect is off. Vision Pro returns to its usual Wi-Fi.");
        ResetDirect();
      }
      else if (line.StartsWith("DIRECT_WIFI_FAILED", StringComparison.Ordinal)) {
        directFailure = line.Length > 19 ? line.Substring(19) : "unknown error";
        Footer("Direct connect could not start. See Activity log for details.");
      }
    }
    string DirectSsid { get { return directAdopted ?? directSettings.Ssid; } }
    // Written from the user's side: what is happening, and the one next step.
    void UpdateDirect(bool running) {
      string pc = Environment.MachineName;
      const string lanHint = "Start the host. On Vision Pro, select this PC or add the address above with Secure QUIC. Compare the pairing code on both devices.";
      string state, status, hint = lanHint;
      bool showJoin = false;
      if (running && directOn) {
        bool joined = directClients > 0;
        state = streaming ? "On · streaming to Vision Pro" : joined ? "On · Vision Pro joined" : "On · waiting for Vision Pro to join";
        showJoin = !joined;
        var details = new List<string>();
        if (directInternet != null && directInternet.StartsWith("shared ")) details.Add("The headset has internet through this PC.");
        else details.Add("Local only: no internet on the headset while it is joined.");
        if (stationMhz > 0 && stationMhz < 3000)
          details.Add("This PC's Wi-Fi is on 2.4 GHz, which slows the headset too. Use Ethernet or 5 GHz Wi-Fi on this PC.");
        if (!directFirewallPresent) details.Add("Windows Firewall is blocking the headset: click Allow through firewall.");
        status = String.Join(" ", details);
        hint = streaming ? "Connected directly to this PC. Turning direct connect off returns Vision Pro to its usual Wi-Fi."
          : joined ? "In NDisplay on Vision Pro, select “" + pc + "”: it shows as online. The first time only, confirm the same pairing code on both devices."
          : "On Vision Pro, join “" + DirectSsid + "” with the password shown. NDisplay then finds “" + pc + "” by itself.";
      } else if (running) {
        state = "Turning on…";
        status = "This takes a few seconds.";
      } else {
        state = "Off";
        status = directFailure != null ? "Could not turn on: " + directFailure + ". Check that Wi-Fi is on and airplane mode is off."
          : directProbe ?? "Checking Wi-Fi…";
      }
      Text("DirectState", state);
      Text("NetworkStatus", status);
      Text("ConnectHint", hint);
      Find<Border>("JoinPanel").Visibility = showJoin ? Visibility.Visible : Visibility.Collapsed;
      Find<TextBlock>("DirectIntro").Visibility = running ? Visibility.Collapsed : Visibility.Visible;
      if (showJoin) {
        Text("JoinSsid", DirectSsid);
        var password = Find<TextBox>("JoinPassword");
        string shown = directAdopted != null ? "as set in Windows Settings → Mobile hotspot" : directSettings.Password;
        if (password.Text != shown) password.Text = shown;
        password.FontSize = directAdopted != null ? 13 : 22;
      }
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
      bool canConfigure = ready && !running && !busy;
      foreach (string name in new [] {"DisplayChoice","FpsChoice","BitrateChoice","EncoderChoice","StereoChoice"}) Find<ComboBox>(name).IsEnabled = canConfigure;
      foreach (string name in new [] {"AllowInput","SendAudio","ScreenOff"}) Find<CheckBox>(name).IsEnabled = canConfigure;
      Find<CheckBox>("GameDepth").IsEnabled = canConfigure && StereoLayout == "none";
      Find<ComboBox>("StereoSizeChoice").IsEnabled =
        canConfigure && (StereoLayout == "sbs" || StereoLayout == "sbs-half");
      Find<Button>("StartButton").IsEnabled = ready && !running && !busy;
      Find<Button>("RefreshDisplaysButton").IsEnabled = ready && !running && !busy;
      Find<Button>("StopButton").IsEnabled = (running || controller.InputAvailable) && !busy;
      Find<Button>("ReceiverButton").IsEnabled = running && !busy;
      Find<Button>("DisableInputButton").IsEnabled = running && controller.InputAvailable && !inputDisabled && !busy;
      Find<Button>("DisableInputButton").Visibility = controller.InputAvailable ? Visibility.Visible : Visibility.Collapsed;
      // The helper exited without a STOPPED line (crash or Windows ended it).
      if (lastDirect && !direct && directOn) { Footer("Direct connect stopped unexpectedly. See Activity log."); ResetDirect(); }
      lastDirect = direct;
      var toggle = Find<CheckBox>("DirectToggle");
      toggle.IsChecked = direct;
      toggle.IsEnabled = ready && !busy && wifiAvailable;
      Find<Button>("FirewallButton").IsEnabled = ready && !busy && (!firewallPresent || !directFirewallPresent);
      foreach (string name in new [] {"SsidText", "BandChoice", "WifiPassword", "NewPasswordButton"})
        ((Control)window.FindName(name)).IsEnabled = !direct && !busy;
      UpdateDirect(direct);
      string status = busy ? "Working…" : running ? (streaming ? "Streaming" : "Waiting for receiver") : "Host stopped";
      Text("StatusBadge", status);
      tray.SetStatus(status);
      Find<Ellipse>("StatusDot").Fill = new SolidColorBrush(running ? Color.FromRgb(91, 218, 166) : Color.FromRgb(153,166,186));
      string address = direct && directOn
        ? (directAddress == null ? HostController.LocalAddress(controller.Port, true) : directAddress + ":" + controller.Port)
        : lanAddress == "unavailable" ? "No LAN IPv4 address"
        : lanAddress != null ? lanAddress + ":" + controller.Port
        : HostController.LocalAddress(controller.Port, false);
      Find<TextBox>("AddressText").Text = address;
      Find<Button>("CopyAddressButton").IsEnabled = address.Contains(":");
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
      closing = true; busy = true; timer.Stop(); Footer("Stopping capture, input and direct connect…"); Refresh();
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
