using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;

namespace Dokan.BootstrapperUX.Controls
{
	public class ProgressRing : Control
	{
		const string STATE_Inactive = "Inactive";
		const string STATE_Active = "Active";

		public static readonly DependencyProperty TemplateSettingsProperty = DependencyProperty.Register("TemplateSettings", typeof(ProgressTemplateSettings), typeof(ProgressRing), new FrameworkPropertyMetadata(null, FrameworkPropertyMetadataOptions.AffectsMeasure | FrameworkPropertyMetadataOptions.AffectsArrange | FrameworkPropertyMetadataOptions.AffectsRender));

		public ProgressTemplateSettings TemplateSettings
		{
			get { return (ProgressTemplateSettings)this.GetValue(TemplateSettingsProperty); }
			set { this.SetValue(TemplateSettingsProperty, value); }
		}

		static ProgressRing()
		{
			DefaultStyleKeyProperty.OverrideMetadata(typeof(ProgressRing), new FrameworkPropertyMetadata(typeof(ProgressRing)));
		}

		public ProgressRing()
		{
			this.TemplateSettings = new ProgressTemplateSettings();

			this.IsEnabledChanged += ProgressRing_IsEnabledChanged;
			this.Loaded += ProgressRing_Loaded;
		}

		void ProgressRing_Loaded(object sender, RoutedEventArgs e)
		{
			if(this.IsEnabled)
			{
				VisualStateManager.GoToState(this, STATE_Active, true);
			}
			else
			{
				VisualStateManager.GoToState(this, STATE_Inactive, true);
			}
		}

		void ProgressRing_IsEnabledChanged(object sender, DependencyPropertyChangedEventArgs e)
		{
			if((bool)e.NewValue)
			{
				VisualStateManager.GoToState(this, STATE_Active, true);
			}
			else
			{
				VisualStateManager.GoToState(this, STATE_Inactive, true);
			}
		}
	}
}
