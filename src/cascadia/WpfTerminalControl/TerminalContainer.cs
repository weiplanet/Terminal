﻿// <copyright file="TerminalContainer.cs" company="Microsoft Corporation">
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
// </copyright>

namespace Microsoft.Terminal.Wpf
{
    using System;
    using System.Runtime.InteropServices;
    using System.Windows;
    using System.Windows.Interop;
    using System.Windows.Media;
    using System.Windows.Threading;

    /// <summary>
    /// The container class that hosts the native hwnd terminal.
    /// </summary>
    /// <remarks>
    /// This class is only left public since xaml cannot work with internal classes.
    /// </remarks>
    public class TerminalContainer : HwndHost
    {
        private static void UnpackKeyMessage(IntPtr wParam, IntPtr lParam, out ushort vkey, out ushort scanCode, out ushort flags)
        {
            ulong scanCodeAndFlags = (((ulong)lParam) & 0xFFFF0000) >> 16;
            scanCode = (ushort)(scanCodeAndFlags & 0x00FFu);
            flags = (ushort)(scanCodeAndFlags & 0xFF00u);
            vkey = (ushort)wParam;
        }

        private static void UnpackCharMessage(IntPtr wParam, IntPtr lParam, out char character, out ushort scanCode, out ushort flags)
        {
            UnpackKeyMessage(wParam, lParam, out ushort vKey, out scanCode, out flags);
            character = (char)vKey;
        }

        private ITerminalConnection connection;
        private IntPtr hwnd;
        private IntPtr terminal;
        private DispatcherTimer blinkTimer;
        private NativeMethods.ScrollCallback scrollCallback;
        private NativeMethods.WriteCallback writeCallback;

        /// <summary>
        /// Initializes a new instance of the <see cref="TerminalContainer"/> class.
        /// </summary>
        public TerminalContainer()
        {
            this.MessageHook += this.TerminalContainer_MessageHook;
            this.GotFocus += this.TerminalContainer_GotFocus;
            this.Focusable = true;

            var blinkTime = NativeMethods.GetCaretBlinkTime();

            if (blinkTime != uint.MaxValue)
            {
                this.blinkTimer = new DispatcherTimer();
                this.blinkTimer.Interval = TimeSpan.FromMilliseconds(blinkTime);
                this.blinkTimer.Tick += (_, __) =>
                {
                    if (this.terminal != IntPtr.Zero)
                    {
                        NativeMethods.TerminalBlinkCursor(this.terminal);
                    }
                };
            }
        }

        /// <summary>
        /// Event that is fired when the terminal buffer scrolls from text output.
        /// </summary>
        internal event EventHandler<(int viewTop, int viewHeight, int bufferSize)> TerminalScrolled;

        /// <summary>
        /// Event that is fired when the user engages in a mouse scroll over the terminal hwnd.
        /// </summary>
        internal event EventHandler<int> UserScrolled;

        /// <summary>
        /// Gets the character rows available to the terminal.
        /// </summary>
        internal int Rows { get; private set; }

        /// <summary>
        /// Gets the character columns available to the terminal.
        /// </summary>
        internal int Columns { get; private set; }

        /// <summary>
        /// Gets the window handle of the terminal.
        /// </summary>
        internal IntPtr Hwnd => this.hwnd;

        /// <summary>
        /// Sets the connection to the terminal backend.
        /// </summary>
        internal ITerminalConnection Connection
        {
            private get
            {
                return this.connection;
            }

            set
            {
                if (this.connection != null)
                {
                    this.connection.TerminalOutput -= this.Connection_TerminalOutput;
                }

                this.connection = value;
                this.connection.TerminalOutput += this.Connection_TerminalOutput;
                this.connection.Start();
            }
        }

        /// <summary>
        /// Manually invoke a scroll of the terminal buffer.
        /// </summary>
        /// <param name="viewTop">The top line to show in the terminal.</param>
        internal void UserScroll(int viewTop)
        {
            NativeMethods.TerminalUserScroll(this.terminal, viewTop);
        }

        /// <summary>
        /// Sets the theme for the terminal. This includes font family, size, color, as well as background and foreground colors.
        /// </summary>
        /// <param name="theme">The color theme for the terminal to use.</param>
        /// <param name="fontFamily">The font family to use in the terminal.</param>
        /// <param name="fontSize">The font size to use in the terminal.</param>
        internal void SetTheme(TerminalTheme theme, string fontFamily, short fontSize)
        {
            var dpiScale = VisualTreeHelper.GetDpi(this);

            NativeMethods.TerminalSetTheme(this.terminal, theme, fontFamily, fontSize, (int)dpiScale.PixelsPerInchX);

            this.TriggerResize(this.RenderSize);
        }

        /// <summary>
        /// Gets the selected text from the terminal renderer and clears the selection.
        /// </summary>
        /// <returns>The selected text, empty if no text is selected.</returns>
        internal string GetSelectedText()
        {
            if (NativeMethods.TerminalIsSelectionActive(this.terminal))
            {
                return NativeMethods.TerminalGetSelection(this.terminal);
            }

            return string.Empty;
        }

        /// <summary>
        /// Triggers a refresh of the terminal with the given size.
        /// </summary>
        /// <param name="renderSize">Size of the rendering window.</param>
        /// <returns>Tuple with rows and columns.</returns>
        internal (int rows, int columns) TriggerResize(Size renderSize)
        {
            var dpiScale = VisualTreeHelper.GetDpi(this);

            NativeMethods.COORD dimensions;
            NativeMethods.TerminalTriggerResize(this.terminal, renderSize.Width * dpiScale.DpiScaleX, renderSize.Height * dpiScale.DpiScaleY, out dimensions);

            this.Rows = dimensions.Y;
            this.Columns = dimensions.X;

            this.connection?.Resize((uint)dimensions.Y, (uint)dimensions.X);
            return (dimensions.Y, dimensions.X);
        }

        /// <summary>
        /// Resizes the terminal.
        /// </summary>
        /// <param name="rows">Number of rows to show.</param>
        /// <param name="columns">Number of columns to show.</param>
        internal void Resize(uint rows, uint columns)
        {
            NativeMethods.COORD dimensions = new NativeMethods.COORD
            {
                X = (short)columns,
                Y = (short)rows,
            };

            NativeMethods.TerminalResize(this.terminal, dimensions);

            this.Rows = dimensions.Y;
            this.Columns = dimensions.X;

            this.connection?.Resize((uint)dimensions.Y, (uint)dimensions.X);
        }

        /// <inheritdoc/>
        protected override void OnDpiChanged(DpiScale oldDpi, DpiScale newDpi)
        {
            if (this.terminal != IntPtr.Zero)
            {
                NativeMethods.TerminalDpiChanged(this.terminal, (int)(NativeMethods.USER_DEFAULT_SCREEN_DPI * newDpi.DpiScaleX));
            }
        }

        /// <inheritdoc/>
        protected override HandleRef BuildWindowCore(HandleRef hwndParent)
        {
            var dpiScale = VisualTreeHelper.GetDpi(this);
            NativeMethods.CreateTerminal(hwndParent.Handle, out this.hwnd, out this.terminal);

            this.scrollCallback = this.OnScroll;
            this.writeCallback = this.OnWrite;

            NativeMethods.TerminalRegisterScrollCallback(this.terminal, this.scrollCallback);
            NativeMethods.TerminalRegisterWriteCallback(this.terminal, this.writeCallback);

            // If the saved DPI scale isn't the default scale, we push it to the terminal.
            if (dpiScale.PixelsPerInchX != NativeMethods.USER_DEFAULT_SCREEN_DPI)
            {
                NativeMethods.TerminalDpiChanged(this.terminal, (int)dpiScale.PixelsPerInchX);
            }

            if (NativeMethods.GetFocus() == this.hwnd)
            {
                this.blinkTimer?.Start();
            }
            else
            {
                NativeMethods.TerminalSetCursorVisible(this.terminal, false);
            }

            return new HandleRef(this, this.hwnd);
        }

        /// <inheritdoc/>
        protected override void DestroyWindowCore(HandleRef hwnd)
        {
            NativeMethods.DestroyTerminal(this.terminal);
            this.terminal = IntPtr.Zero;
        }

        private void TerminalContainer_GotFocus(object sender, RoutedEventArgs e)
        {
            e.Handled = true;
            NativeMethods.SetFocus(this.hwnd);
        }

        private IntPtr TerminalContainer_MessageHook(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
        {
            if (hwnd == this.hwnd)
            {
                switch ((NativeMethods.WindowMessage)msg)
                {
                    case NativeMethods.WindowMessage.WM_SETFOCUS:
                        NativeMethods.TerminalSetFocus(this.terminal);
                        this.blinkTimer?.Start();
                        break;
                    case NativeMethods.WindowMessage.WM_KILLFOCUS:
                        NativeMethods.TerminalKillFocus(this.terminal);
                        this.blinkTimer?.Stop();
                        NativeMethods.TerminalSetCursorVisible(this.terminal, false);
                        break;
                    case NativeMethods.WindowMessage.WM_MOUSEACTIVATE:
                        this.Focus();
                        NativeMethods.SetFocus(this.hwnd);
                        break;
                    case NativeMethods.WindowMessage.WM_SYSKEYDOWN: // fallthrough
                    case NativeMethods.WindowMessage.WM_KEYDOWN:
                        {
                            // WM_KEYDOWN lParam layout documentation: https://docs.microsoft.com/en-us/windows/win32/inputdev/wm-keydown
                            NativeMethods.TerminalSetCursorVisible(this.terminal, true);

                            UnpackKeyMessage(wParam, lParam, out ushort vkey, out ushort scanCode, out ushort flags);
                            NativeMethods.TerminalSendKeyEvent(this.terminal, vkey, scanCode, flags, true);
                            this.blinkTimer?.Start();
                            break;
                        }

                    case NativeMethods.WindowMessage.WM_SYSKEYUP: // fallthrough
                    case NativeMethods.WindowMessage.WM_KEYUP:
                        {
                            // WM_KEYUP lParam layout documentation: https://docs.microsoft.com/en-us/windows/win32/inputdev/wm-keyup
                            UnpackKeyMessage(wParam, lParam, out ushort vkey, out ushort scanCode, out ushort flags);
                            NativeMethods.TerminalSendKeyEvent(this.terminal, (ushort)wParam, scanCode, flags, false);
                            break;
                        }

                    case NativeMethods.WindowMessage.WM_CHAR:
                        {
                            // WM_CHAR lParam layout documentation: https://docs.microsoft.com/en-us/windows/win32/inputdev/wm-char
                            UnpackCharMessage(wParam, lParam, out char character, out ushort scanCode, out ushort flags);
                            NativeMethods.TerminalSendCharEvent(this.terminal, character, scanCode, flags);
                            break;
                        }

                    case NativeMethods.WindowMessage.WM_WINDOWPOSCHANGED:
                        var windowpos = (NativeMethods.WINDOWPOS)Marshal.PtrToStructure(lParam, typeof(NativeMethods.WINDOWPOS));
                        if (((NativeMethods.SetWindowPosFlags)windowpos.flags).HasFlag(NativeMethods.SetWindowPosFlags.SWP_NOSIZE))
                        {
                            break;
                        }

                        NativeMethods.TerminalTriggerResize(this.terminal, windowpos.cx, windowpos.cy, out var dimensions);

                        this.connection?.Resize((uint)dimensions.Y, (uint)dimensions.X);
                        this.Columns = dimensions.X;
                        this.Rows = dimensions.Y;

                        break;
                    case NativeMethods.WindowMessage.WM_MOUSEWHEEL:
                        var delta = (short)(((long)wParam) >> 16);
                        this.UserScrolled?.Invoke(this, delta);
                        break;
                }
            }

            return IntPtr.Zero;
        }

        private void LeftClickHandler(int lParam)
        {
            var altPressed = NativeMethods.GetKeyState((int)NativeMethods.VirtualKey.VK_MENU) < 0;
            var x = (short)(((int)lParam << 16) >> 16);
            var y = (short)((int)lParam >> 16);
            NativeMethods.COORD cursorPosition = new NativeMethods.COORD()
            {
                X = x,
                Y = y,
            };

            NativeMethods.TerminalStartSelection(this.terminal, cursorPosition, altPressed);
        }

        private void MouseMoveHandler(int wParam, int lParam)
        {
            if (((int)wParam & 0x0001) == 1)
            {
                var x = (short)(((int)lParam << 16) >> 16);
                var y = (short)((int)lParam >> 16);
                NativeMethods.COORD cursorPosition = new NativeMethods.COORD()
                {
                    X = x,
                    Y = y,
                };
                NativeMethods.TerminalMoveSelection(this.terminal, cursorPosition);
            }
        }

        private void Connection_TerminalOutput(object sender, TerminalOutputEventArgs e)
        {
            if (this.terminal != IntPtr.Zero)
            {
                NativeMethods.TerminalSendOutput(this.terminal, e.Data);
            }
        }

        private void OnScroll(int viewTop, int viewHeight, int bufferSize)
        {
            this.TerminalScrolled?.Invoke(this, (viewTop, viewHeight, bufferSize));
        }

        private void OnWrite(string data)
        {
            this.connection?.WriteInput(data);
        }
    }
}
