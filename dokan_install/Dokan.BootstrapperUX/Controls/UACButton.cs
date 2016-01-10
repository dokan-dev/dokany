using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace Dokan.BootstrapperUX.Controls
{
	///
	/// Provides a WPF button that displays a UAC Shield icon when required. Heavily modified from: http://sohotechnology.wordpress.com/2009/11/22/displaying-the-uac-shield-in-wpf-with-an-adorner/
	///
	[TemplatePart(Name = PART_Icon, Type = typeof(Image))]
	public sealed class UACButton : Button
	{
		[DllImport("Shell32.dll", SetLastError = false)]
		public static extern Int32 SHGetStockIconInfo(SHSTOCKICONID siid, SHGSI uFlags, ref SHSTOCKICONINFO psii);

		public enum SHSTOCKICONID : uint
		{
			SIID_SHIELD = 77
		}

		[Flags]
		public enum SHGSI : uint
		{
			SHGSI_ICON = 0x000000100,
			SHGSI_SMALLICON = 0x000000001
		}

		[StructLayoutAttribute(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
		public struct SHSTOCKICONINFO
		{
			public UInt32 cbSize;
			public IntPtr hIcon;
			public Int32 iSysIconIndex;
			public Int32 iIcon;

			[MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)]
			public string szPath;
		}

		const string PART_Icon = "PART_Icon";

		public static readonly DependencyProperty ShowShieldProperty = DependencyProperty.Register("ShowShield", typeof(bool), typeof(UACButton), new PropertyMetadata(Environment.OSVersion.Version.Major >= 6, OnShowShieldChanged));
		public static readonly DependencyProperty ShieldIconProperty;
		public static readonly DependencyProperty ShieldIconDisabledProperty;

		Image m_partIcon;

		public ImageSource ShieldIcon
		{
			get { return (BitmapSource)this.GetValue(ShieldIconProperty); }
			set { this.SetValue(ShieldIconProperty, value); }
		}

		public ImageSource ShieldIconDisabled
		{
			get { return (BitmapSource)this.GetValue(ShieldIconDisabledProperty); }
			set { this.SetValue(ShieldIconDisabledProperty, value); }
		}

		public bool ShowShield
		{
			get { return (bool)this.GetValue(ShowShieldProperty); }
			set { this.SetValue(ShowShieldProperty, value); }
		}

		///
		/// Initializes static members of the class.
		///
		static UACButton()
		{
			DefaultStyleKeyProperty.OverrideMetadata(typeof(UACButton), new FrameworkPropertyMetadata(typeof(UACButton)));

			BitmapSource shield = null;
			BitmapSource shieldDisabled = null;

			if(Environment.OSVersion.Version.Major >= 6)
			{
				try
				{
					SHSTOCKICONINFO iconResult = new SHSTOCKICONINFO();
					iconResult.cbSize = (uint)System.Runtime.InteropServices.Marshal.SizeOf(iconResult);

					SHGetStockIconInfo(SHSTOCKICONID.SIID_SHIELD, SHGSI.SHGSI_ICON | SHGSI.SHGSI_SMALLICON, ref iconResult);

					using(System.Drawing.Icon ico = System.Drawing.Icon.FromHandle(iconResult.hIcon))
					{
						shield = Imaging.CreateBitmapSourceFromHIcon(ico.Handle, new Int32Rect(0, 0, ico.Width, ico.Height), BitmapSizeOptions.FromEmptyOptions());
					}

					BitmapSource pixelsBmp;

					if(shield.Format != PixelFormats.Bgra32)
					{
						pixelsBmp = new FormatConvertedBitmap(shield, PixelFormats.Bgra32, null, 0);
					}
					else
					{
						pixelsBmp = shield;
					}

					byte[] pixelData = new byte[pixelsBmp.PixelWidth * pixelsBmp.PixelHeight * 4];

					pixelsBmp.CopyPixels(pixelData, pixelsBmp.PixelWidth * 4, 0);

					for(int i = 0; i < pixelData.Length; i += 4)
					{
						float blue = SRGBToLinear(pixelData[i] * (1.0f / 255.0f));
						float green = SRGBToLinear(pixelData[i + 1] * (1.0f / 255.0f));
						float red = SRGBToLinear(pixelData[i + 2] * (1.0f / 255.0f));

						float lum = 0.2126f * red + 0.7152f * green + 0.0722f * blue;

						// convert back to srgb

						if(lum <= 0.0031308f)
						{
							lum = 12.92f * lum;
						}
						else
						{
							lum = (float)(1.055 * Math.Pow(lum, 1.0 / 2.4) - 0.055);
						}

						// clamp to 0 - 1
						lum = Math.Max(Math.Min(1.0f, lum), 0.0f);

						byte srgbLum = (byte)(lum * 255);

						pixelData[i] = srgbLum;
						pixelData[i + 1] = srgbLum;
						pixelData[i + 2] = srgbLum;
					}

					shieldDisabled = BitmapImage.Create(pixelsBmp.PixelWidth, pixelsBmp.PixelHeight, pixelsBmp.DpiX, pixelsBmp.DpiY, PixelFormats.Bgra32, null, pixelData, pixelsBmp.PixelWidth * 4);
				}
				catch(Exception ex)
				{
					System.Diagnostics.Debug.WriteLine(ex.ToString());
				}
			}

			ShieldIconProperty = DependencyProperty.Register("ShieldIcon", typeof(ImageSource), typeof(UACButton), new FrameworkPropertyMetadata(shield));
			ShieldIconDisabledProperty = DependencyProperty.Register("ShieldIconDisabled", typeof(ImageSource), typeof(UACButton), new FrameworkPropertyMetadata(shieldDisabled));
		}

		// http://en.wikipedia.org/wiki/Grayscale#Converting_color_to_grayscale
		static float SRGBToLinear(float srgb)
		{
			if(srgb <= 0.04045f)
			{
				srgb = srgb / 12.92f;
			}
			else
			{
				srgb = (srgb + 0.055f) / 1.055f;
			}

			return srgb;
		}

		///
		/// Initializes a new instance of the class.
		///
		public UACButton()
		{
			this.IsEnabledChanged += UACButton_IsEnabledChanged;
		}

		void UACButton_IsEnabledChanged(object sender, DependencyPropertyChangedEventArgs e)
		{
			if(m_partIcon != null)
			{
				//BindingOperations.ClearBinding(m_partIcon, Image.SourceProperty);

				Binding binding;

				if((bool)e.NewValue)
				{
					binding = new Binding("ShieldIcon");
				}
				else
				{
					binding = new Binding("ShieldIconDisabled");
				}

				binding.Source = this;

				BindingOperations.SetBinding(m_partIcon, Image.SourceProperty, binding);
			}
		}

		public override void OnApplyTemplate()
		{
			base.OnApplyTemplate();

			m_partIcon = this.GetTemplateChild(PART_Icon) as Image;

			if(m_partIcon != null)
			{
				Binding binding;

				if(this.IsEnabled)
				{
					binding = new Binding("ShieldIcon");
				}
				else
				{
					binding = new Binding("ShieldIconDisabled");
				}

				binding.Source = this;

				BindingOperations.SetBinding(m_partIcon, Image.SourceProperty, binding);

				m_partIcon.Visibility = this.ShowShield ? Visibility.Visible : Visibility.Collapsed;
			}
		}

		private static void OnShowShieldChanged(DependencyObject obj, DependencyPropertyChangedEventArgs e)
		{
			UACButton btn = (UACButton)obj;

			if(btn.m_partIcon != null)
			{
				if((bool)e.NewValue)
				{
					btn.m_partIcon.Visibility = Visibility.Visible;
				}
				else
				{
					btn.m_partIcon.Visibility = Visibility.Collapsed;
				}
			}
		}
	}
}
