using System;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using Drawing = System.Drawing;
using Forms = System.Windows.Forms;

namespace WvdHost {
  // NotifyIcon uses the WPF thread's native message loop. Keep its icon and
  // context menu alive until the main window has finished graceful shutdown.
  public sealed class TrayIcon : IDisposable {
    readonly Forms.NotifyIcon notification;
    readonly Forms.ContextMenuStrip menu;
    readonly Drawing.Icon icon;
    public ImageSource WindowIcon { get; private set; }
    public bool Visible { get { return notification.Visible; } }

    public TrayIcon(Action open, Action exit) {
      icon = CreateIcon();
      WindowIcon = Imaging.CreateBitmapSourceFromHIcon(icon.Handle, Int32Rect.Empty,
        System.Windows.Media.Imaging.BitmapSizeOptions.FromEmptyOptions());
      WindowIcon.Freeze();
      menu = new Forms.ContextMenuStrip();
      menu.Items.Add("Open " + HostInfo.Name, null, delegate { open(); });
      menu.Items.Add(new Forms.ToolStripSeparator());
      menu.Items.Add("Exit " + HostInfo.Name, null, delegate { exit(); });
      notification = new Forms.NotifyIcon { Icon = icon, Text = HostInfo.Full + " · Windows display host",
        ContextMenuStrip = menu, Visible = true };
      notification.MouseClick += delegate(object sender, Forms.MouseEventArgs e) {
        if (e.Button == Forms.MouseButtons.Left) open();
      };
      notification.DoubleClick += delegate { open(); };
    }
    public void SetStatus(string status) {
      string text = HostInfo.Product + " · " + status;
      notification.Text = text.Length > 63 ? text.Substring(0, 63) : text;
    }
    public void Dispose() {
      notification.Visible = false;
      notification.Dispose(); menu.Dispose(); icon.Dispose();
    }
    static Drawing.Icon CreateIcon() {
      using (var bitmap = new Drawing.Bitmap(32, 32)) {
        using (var graphics = Drawing.Graphics.FromImage(bitmap))
        using (var background = new Drawing.SolidBrush(Drawing.Color.FromArgb(74, 118, 237)))
        using (var outline = new Drawing.Pen(Drawing.Color.White, 2)) {
          graphics.SmoothingMode = Drawing.Drawing2D.SmoothingMode.AntiAlias;
          graphics.Clear(Drawing.Color.Transparent);
          graphics.FillEllipse(background, 0, 0, 31, 31);
          graphics.DrawRectangle(outline, 7, 8, 18, 12);
          graphics.DrawLine(outline, 16, 21, 16, 25);
          graphics.DrawLine(outline, 11, 25, 21, 25);
        }
        IntPtr handle = bitmap.GetHicon();
        try {
          using (var borrowed = Drawing.Icon.FromHandle(handle)) return (Drawing.Icon)borrowed.Clone();
        } finally { DestroyIcon(handle); }
      }
    }
    [DllImport("user32.dll")] static extern bool DestroyIcon(IntPtr handle);
  }
}
