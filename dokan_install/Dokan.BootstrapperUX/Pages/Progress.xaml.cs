using Microsoft.Tools.WindowsInstallerXml.Bootstrapper;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Documents;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Navigation;
using System.Windows.Shapes;

namespace Dokan.BootstrapperUX.Pages
{
	/// <summary>
	/// Interaction logic for Progress.xaml
	/// </summary>
	public partial class Progress : UserControl, INavPage, IBootstrapperApplicationAware
	{
		DokanBootstrapperApplication m_app;

		public DokanBootstrapperApplication Application
		{
			get { return m_app; }
			set
			{
				if(m_app != null)
				{
					m_app.ExecutePackageBegin -= Installer_ExecutePackageBegin;
					m_app.ExecuteProgress -= Installer_ExecuteProgress;
					m_app.CacheAcquireBegin -= Installer_CacheAcquireBegin;
					m_app.CacheAcquireProgress -= Installer_CacheAcquireProgress;
				}

				m_app = value;

				if(m_app != null)
				{
					m_app.ExecutePackageBegin += Installer_ExecutePackageBegin;
					m_app.ExecuteProgress += Installer_ExecuteProgress;
					m_app.CacheAcquireBegin += Installer_CacheAcquireBegin;
					m_app.CacheAcquireProgress += Installer_CacheAcquireProgress;
				}
			}
		}
		NavButtonsUsed INavPage.Buttons
		{
			get { return NavButtonsUsed.Cancel; }
		}

		Pages INavPage.NextPage
		{
			get { return Pages.None; }
		}

		INavPageContainer INavPage.NavContainer
		{
			get;
			set;
		}

		public Progress()
		{
			InitializeComponent();
		}

		void INavPage.OnPageHiding()
		{
		}

		void INavPage.OnPageHidden()
		{
		}

		void INavPage.OnPageShowing()
		{
		}

		void INavPage.OnPageShown()
		{
		}

		void Installer_CacheAcquireProgress(object sender, CacheAcquireProgressEventArgs e)
		{
			this.Dispatcher.BeginInvoke(new Action(() =>
			{
				m_progressBarPackage.Value = e.OverallPercentage / 100.0;
			}));
		}

		void Installer_CacheAcquireBegin(object sender, CacheAcquireBeginEventArgs e)
		{
			this.Dispatcher.BeginInvoke(new Action(() =>
			{
				PackageInstallationInfo pkgProps;
				m_app.Packages.TryGetValue(e.PackageOrContainerId, out pkgProps);

				m_txtProgress.Text = string.Format("Acquiring {0}...", pkgProps != null ? pkgProps.Properties.DisplayName : e.PackageOrContainerId);
			}));
		}

		void Installer_ExecuteProgress(object sender, ExecuteProgressEventArgs e)
		{
			this.Dispatcher.Invoke(new Action(() =>
			{
				m_progressBarPackage.Value = e.ProgressPercentage / 100.0;
				m_progressBarOverall.Value = e.OverallPercentage / 100.0;
			}));
		}

		void Installer_ExecutePackageBegin(object sender, ExecutePackageBeginEventArgs e)
		{
			this.Dispatcher.BeginInvoke(new Action(() =>
			{
				PackageInstallationInfo pkgProps;
				m_app.Packages.TryGetValue(e.PackageId, out pkgProps);

				string displayName;

				if(pkgProps != null && !string.IsNullOrWhiteSpace(pkgProps.Properties.DisplayName))
				{
					displayName = pkgProps.Properties.DisplayName;
				}
				else
				{
					displayName = "package " + e.PackageId;
				}

				// detect rollback: http://stackoverflow.com/questions/15408323/wix-bootstrapper-rollback-notification
				if(e.ShouldExecute == false)
				{
					m_txtProgress.Text = string.Format("Rolling back {0}...", displayName);
				}
				else
				{
					if(m_app.InstallationType == LaunchAction.Install)
					{
						m_txtProgress.Text = string.Format("Installing {0}...", displayName);
					}
					else if(m_app.InstallationType == LaunchAction.UpdateReplace || m_app.InstallationType == LaunchAction.UpdateReplaceEmbedded || m_app.InstallationType == LaunchAction.Modify)
					{
						m_txtProgress.Text = string.Format("Updating {0}...", displayName);
					}
					else if(m_app.InstallationType == LaunchAction.Repair)
					{
						m_txtProgress.Text = string.Format("Repairing {0}...", displayName);
					}
					else if(m_app.InstallationType == LaunchAction.Uninstall)
					{
						m_txtProgress.Text = string.Format("Uninstalling {0}...", displayName);
					}
					else
					{
						m_txtProgress.Text = string.Format("Executing action for package {0}...", displayName);
					}
				}
			}));
		}
	}
}
