using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Input;

namespace Dokan.BootstrapperUX
{
	public static class InstallerCommands
	{
		public static readonly RoutedUICommand Install = new RoutedUICommand("Install", "Install", typeof(InstallerCommands));
		public static readonly RoutedUICommand InstallTypical = new RoutedUICommand("Install typical features", "InstallTypical", typeof(InstallerCommands));
		public static readonly RoutedUICommand InstallComplete = new RoutedUICommand("Install everything", "InstallComplete", typeof(InstallerCommands));
		public static readonly RoutedUICommand Uninstall = new RoutedUICommand("Uninstall", "Uninstall", typeof(InstallerCommands));
		public static readonly RoutedUICommand Repair = new RoutedUICommand("Repair", "Repair", typeof(InstallerCommands));
		public static readonly RoutedUICommand Modify = new RoutedUICommand("Modify", "Modify", typeof(InstallerCommands));
		public static readonly RoutedUICommand ChooseFeatures = new RoutedUICommand("Choose features", "ChooseFeatures", typeof(InstallerCommands));
		public static readonly RoutedUICommand Back = new RoutedUICommand("Back", "Back", typeof(InstallerCommands));
		public static readonly RoutedUICommand Next = new RoutedUICommand("Next", "Next", typeof(InstallerCommands));
		public static readonly RoutedUICommand Restart = new RoutedUICommand("Restart", "Restart", typeof(InstallerCommands));
		public static readonly RoutedUICommand Cancel = new RoutedUICommand("Cancel", "Cancel", typeof(InstallerCommands));
	}
}
