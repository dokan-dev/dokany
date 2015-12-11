using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Data;

namespace Dokan.BootstrapperUX
{
	public class InverseBoolValueConverter : IValueConverter
	{
		public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
		{
			if(value is bool)
			{
				return !(bool)value;
			}

			return !bool.Parse(value.ToString());
		}

		public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
		{
			if(value is bool)
			{
				return !(bool)value;
			}

			return !bool.Parse(value.ToString());
		}
	}
}
