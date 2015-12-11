using Microsoft.Tools.WindowsInstallerXml.Bootstrapper;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace Dokan.BootstrapperUX
{
	public class InstallationTypeChangedEventArgs : EventArgs
	{
		public LaunchAction OldInstallationType
		{
			get;
			private set;
		}

		public LaunchAction NewInstallationType
		{
			get;
			private set;
		}

		public InstallationTypeChangedEventArgs(LaunchAction oldInstallationType, LaunchAction newInstallationType)
		{
			this.OldInstallationType = oldInstallationType;
			this.NewInstallationType = newInstallationType;
		}
	}
}
