using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows;

namespace Dokan.BootstrapperUX.Pages
{
	public class FeatureItem : DependencyObject, INotifyPropertyChanged
	{
		struct ChildrenState
		{
			int m_childrenChecked;
			int m_childrenCheckedPartial;

			public int ChildrenChecked { get { return m_childrenChecked; } }

			public int ChildrenCheckedPartial { get { return m_childrenCheckedPartial; } }

			public bool HasChecked { get { return m_childrenChecked > 0 || m_childrenCheckedPartial > 0; } }

			public ChildrenState(int childrenChecked, int childrenCheckedPartial)
			{
				m_childrenChecked = childrenChecked;
				m_childrenCheckedPartial = childrenCheckedPartial;
			}
		}

		public static readonly DependencyPropertyKey ChildrenPropertyKey = DependencyProperty.RegisterReadOnly("Children", typeof(FeatureItem[]), typeof(FeatureItem), new PropertyMetadata(null));
		public static readonly DependencyProperty ChildrenProperty = ChildrenPropertyKey.DependencyProperty;

		public static readonly DependencyProperty IsCheckedProperty = DependencyProperty.Register("IsChecked", typeof(bool?), typeof(FeatureItem), new PropertyMetadata(false, OnIsCheckedPropertyChanged, OnCoerceValueIsChecked));
		public static readonly DependencyProperty DisallowAbsentProperty = DependencyProperty.Register("DisallowAbsent", typeof(bool), typeof(FeatureItem), new PropertyMetadata(false, OnDisallowAbsentPropertyChanged));

		FeatureInstallationInfo m_props;
		PropertyChangedEventHandler m_propertyChanged;
		EventHandler<DependencyPropertyChangedEventArgs> m_isCheckedChanged;
		bool m_isInHandler;

		public event PropertyChangedEventHandler PropertyChanged
		{
			add { m_propertyChanged += value; }
			remove { m_propertyChanged -= value; }
		}

		public event EventHandler<DependencyPropertyChangedEventArgs> IsCheckedChanged
		{
			add { m_isCheckedChanged += value; }
			remove { m_isCheckedChanged -= value; }
		}

		public FeatureItem[] Children
		{
			get { return (FeatureItem[])this.GetValue(ChildrenProperty); }
		}

		public bool? IsChecked
		{
			get { return (bool?)this.GetValue(IsCheckedProperty); }
			set
			{
				if(this.DisallowAbsent && value.HasValue && !value.Value)
				{
					throw new ArgumentOutOfRangeException("IsChecked must be true or null if DisallowAbsent is true.");
				}

				this.SetValue(IsCheckedProperty, value);
			}
		}

		public bool DisallowAbsent
		{
			get { return (bool)this.GetValue(DisallowAbsentProperty); }
			set { this.SetValue(DisallowAbsentProperty, value); }
		}

		public string Title
		{
			get { return m_props.Properties.Title; }
		}

		public string Description
		{
			get { return m_props.Properties.Description; }
		}

		bool HasChildren
		{
			get { return this.Children != null && this.Children.Length > 0; }
		}

		public FeatureItem(FeatureInstallationInfo prop, FeatureItem[] children = null)
		{
			m_props = prop;

			this.DisallowAbsent = (prop.Properties.Attributes & FeatureAttributes.UIDisallowAbsent) == FeatureAttributes.UIDisallowAbsent;

			if(this.DisallowAbsent || prop.Properties.Level == 1)
			{
				m_props.RequestedState = Microsoft.Tools.WindowsInstallerXml.Bootstrapper.FeatureState.Local;
				this.IsChecked = true;
			}
			else if(m_props.Properties.Level == 0)
			{
				// TODO: Level 0 features and their children should not be visible in the UI
				m_props.RequestedState = Microsoft.Tools.WindowsInstallerXml.Bootstrapper.FeatureState.Absent;
				this.IsChecked = false;
			}
			else
			{
				switch(prop.CurrentState)
				{
					case Microsoft.Tools.WindowsInstallerXml.Bootstrapper.FeatureState.Absent:
						{
							m_props.RequestedState = Microsoft.Tools.WindowsInstallerXml.Bootstrapper.FeatureState.Absent;
							this.IsChecked = false;
							break;
						}
					case Microsoft.Tools.WindowsInstallerXml.Bootstrapper.FeatureState.Advertised:
					case Microsoft.Tools.WindowsInstallerXml.Bootstrapper.FeatureState.Local:
					case Microsoft.Tools.WindowsInstallerXml.Bootstrapper.FeatureState.Source:
						{
							m_props.RequestedState = Microsoft.Tools.WindowsInstallerXml.Bootstrapper.FeatureState.Local;
							this.IsChecked = true;
							break;
						}
					case Microsoft.Tools.WindowsInstallerXml.Bootstrapper.FeatureState.Unknown:
						{
							if(prop.Properties.Level > 200)
							{
								m_props.RequestedState = Microsoft.Tools.WindowsInstallerXml.Bootstrapper.FeatureState.Absent;
								this.IsChecked = false;
							}
							else
							{
								m_props.RequestedState = Microsoft.Tools.WindowsInstallerXml.Bootstrapper.FeatureState.Local;
								this.IsChecked = true;
							}

							break;
						}
				}
			}

			if(children != null && children.Length > 0)
			{
				this.SetValue(ChildrenPropertyKey, children);

				foreach(var child in children)
				{
					child.IsCheckedChanged += Child_IsCheckedChanged;
				}
			}

			UpdateCheckState();
		}

		private void Child_IsCheckedChanged(object sender, DependencyPropertyChangedEventArgs e)
		{
			if(m_isInHandler)
			{
				return;
			}

			UpdateCheckState();
		}

		ChildrenState CalculateCheckedChildCount()
		{
			int childrenChecked = 0;
			int childrenCheckedPartial = 0;

			foreach(var child in this.Children)
			{
				if(child.IsChecked.HasValue && child.IsChecked.Value)
				{
					++childrenChecked;
				}
				else if(!child.IsChecked.HasValue)
				{
					++childrenCheckedPartial;
				}
			}

			return new ChildrenState(childrenChecked, childrenCheckedPartial);
		}

		void UpdateCheckState()
		{
			if(this.DisallowAbsent)
			{
				if(this.Children == null || this.Children.Length == 0 || CalculateCheckedChildCount().ChildrenChecked == this.Children.Length)
				{
					this.IsChecked = true;
				}
				else
				{
					this.IsChecked = null;
				}
			}
			else
			{
				if(this.Children == null || this.Children.Length == 0)
				{
					// checkbox can only be true or false, if null set to false
					this.IsChecked = this.IsChecked.HasValue ? this.IsChecked : new bool?(false);
				}
				else
				{
					var childState = CalculateCheckedChildCount();

					if(!this.IsChecked.HasValue)
					{
						if(childState.ChildrenChecked == this.Children.Length)
						{
							this.IsChecked = true;
						}
					}
					else if(this.IsChecked.Value)
					{
						if(childState.ChildrenChecked != this.Children.Length)
						{
							this.IsChecked = null;
						}
					}
					else if(childState.HasChecked)
					{
						// This is the same as else if(!this.IsChecked.Value && childState.HasChecked)
						this.IsChecked = null;
					}
				}
			}
		}

		public void LoadLevel(int level)
		{
			if(this.HasChildren)
			{
				foreach(var child in this.Children)
				{
					child.LoadLevel(level);
				}
			}

			if(!this.DisallowAbsent)
			{
				if(this.m_props.Properties.Level > level)
				{
					this.IsChecked = false;
				}
				else
				{
					this.IsChecked = true;
				}
			}
		}

		static void OnDisallowAbsentPropertyChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
		{
			FeatureItem item = (FeatureItem)d;

			if((bool)e.NewValue)
			{
				d.CoerceValue(IsCheckedProperty);
			}
		}

		static void OnIsCheckedPropertyChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
		{
			FeatureItem item = (FeatureItem)d;
			bool? newValue = (bool?)e.NewValue;

			item.m_isInHandler = true;
			item.m_props.RequestedState = newValue.HasValue && !newValue.Value ? Microsoft.Tools.WindowsInstallerXml.Bootstrapper.FeatureState.Absent : Microsoft.Tools.WindowsInstallerXml.Bootstrapper.FeatureState.Local;

			try
			{
				try
				{
					if(item.m_propertyChanged != null)
					{
						item.m_propertyChanged(item, new PropertyChangedEventArgs("IsChecked"));
					}
				}
				catch(Exception ex)
				{
					System.Diagnostics.Debug.WriteLine(ex.ToString());
				}

				try
				{
					if(item.m_isCheckedChanged != null)
					{
						item.m_isCheckedChanged(item, e);
					}
				}
				catch(Exception ex)
				{
					System.Diagnostics.Debug.WriteLine(ex.ToString());
				}

				bool? newVal = (bool?)e.NewValue;

				if(newVal.HasValue && !newVal.Value && item.Children != null)
				{
					foreach(var child in item.Children)
					{
						if(!child.DisallowAbsent)
						{
							child.IsChecked = false;
						}
					}
				}
			}
			finally
			{
				item.m_isInHandler = false;
			}
		}

		static object OnCoerceValueIsChecked(DependencyObject d, object baseValue)
		{
			FeatureItem item = (FeatureItem)d;

			if(item.m_props.Properties.Level == 0)
			{
				return new bool?(false);
			}

			bool? value = (bool?)baseValue;

			if(item.DisallowAbsent)
			{
				if(!item.HasChildren || item.CalculateCheckedChildCount().ChildrenChecked == item.Children.Length)
				{
					return new bool?(true);
				}
				else
				{
					return null;
				}
			}

			// Three state check box goes cyclically from true -> null -> false. We need to verify the transition state is valid.

			// If we don't have children we can only be true or false, if null then skip it and return false
			if(!item.HasChildren && !value.HasValue)
			{
				return new bool?(false);
			}

			if(item.HasChildren)
			{
				// if we have children then we have to check two different transition states
				if(!value.HasValue)
				{
					// skip null if all children are checked
					if(item.CalculateCheckedChildCount().ChildrenChecked == item.Children.Length)
					{
						return new bool?(false);
					}
				}
				else if(value.Value)
				{
					// skip true if not all children are checked
					if(item.CalculateCheckedChildCount().ChildrenChecked != item.Children.Length)
					{
						return null;
					}
				}
			}

			return baseValue;
		}
	}
}
