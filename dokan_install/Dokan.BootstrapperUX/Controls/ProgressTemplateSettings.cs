using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows;

namespace Dokan.BootstrapperUX.Controls
{
	public class ProgressTemplateSettings : DependencyObject
	{
		public static readonly DependencyProperty EllipseDiameterProperty = DependencyProperty.Register("EllipseDiameter", typeof(double), typeof(ProgressTemplateSettings), new FrameworkPropertyMetadata(6.0, FrameworkPropertyMetadataOptions.AffectsMeasure | FrameworkPropertyMetadataOptions.AffectsArrange | FrameworkPropertyMetadataOptions.AffectsRender));
		public static readonly DependencyProperty EllipseOffsetProperty = DependencyProperty.Register("EllipseOffset", typeof(Thickness), typeof(ProgressTemplateSettings), new FrameworkPropertyMetadata(new Thickness(7), FrameworkPropertyMetadataOptions.AffectsMeasure | FrameworkPropertyMetadataOptions.AffectsArrange | FrameworkPropertyMetadataOptions.AffectsRender));
		public static readonly DependencyProperty MaxSideLengthProperty = DependencyProperty.Register("MaxSideLength", typeof(double), typeof(ProgressTemplateSettings), new FrameworkPropertyMetadata(double.PositiveInfinity, FrameworkPropertyMetadataOptions.AffectsMeasure | FrameworkPropertyMetadataOptions.AffectsArrange | FrameworkPropertyMetadataOptions.AffectsRender));

		public double EllipseDiameter
		{
			get { return (double)this.GetValue(EllipseDiameterProperty); }
			set { this.SetValue(EllipseDiameterProperty, value); }
		}

		public Thickness EllipseOffset
		{
			get { return (Thickness)this.GetValue(EllipseOffsetProperty); }
			set { this.SetValue(EllipseOffsetProperty, value); }
		}

		public double MaxSideLength
		{
			get { return (double)this.GetValue(MaxSideLengthProperty); }
			set { this.SetValue(MaxSideLengthProperty, value); }
		}
	}
}
