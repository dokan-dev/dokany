using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace Dokan.BootstrapperUX
{
	public interface IBootstrapperApplicationAware
	{
		DokanBootstrapperApplication Application
		{
			get;
			set;
		}
	}
}
