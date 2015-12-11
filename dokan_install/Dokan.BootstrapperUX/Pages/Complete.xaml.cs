using Microsoft.Deployment.WindowsInstaller;
using Microsoft.Tools.WindowsInstallerXml.Bootstrapper;
using System;
using System.Collections.Generic;
using System.IO;
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
	/// Interaction logic for Complete.xaml
	/// </summary>
	public partial class Complete : UserControl, INavPage, IBootstrapperApplicationAware
	{
		NavButtonsUsed m_buttons = NavButtonsUsed.Close;

		public DokanBootstrapperApplication Application
		{
			get;
			set;
		}

		NavButtonsUsed INavPage.Buttons
		{
			get { return m_buttons; }
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

		public Complete()
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

		private void OpenLogFile_Click(object sender, RoutedEventArgs e)
		{
			if(this.Application == null)
			{
				return;
			}

			// http://stackoverflow.com/questions/10741139/how-to-set-or-get-all-logs-in-a-custom-bootstrapper-application
			string path = this.Application.Engine.StringVariables["WixBundleLog"];

			if(!string.IsNullOrWhiteSpace(path) && File.Exists(path))
			{
				System.Diagnostics.ProcessStartInfo info = new System.Diagnostics.ProcessStartInfo();
				info.Verb = "open";
				info.FileName = path;
				info.UseShellExecute = true;

				using(var proc = System.Diagnostics.Process.Start(info))
				{
				}
			}
		}

		public void LoadState(ApplyCompleteEventArgs e)
		{
			if(this.Application == null)
			{
				return;
			}

			m_buttons = NavButtonsUsed.Close;

			if(e.Restart == ApplyRestart.RestartRequired
				|| e.Restart == ApplyRestart.RestartInitiated
				|| this.Application.Command.Restart == Restart.Always)
			{
				m_buttons |= NavButtonsUsed.Restart;
			}

			switch(this.Application.InstallationType)
			{
				case LaunchAction.Uninstall:
					{
						switch(this.Application.InstallationResult)
						{
							case ActionResult.UserExit:
								{
									m_txtUninstallFinishedCancel.Visibility = Visibility.Visible;
									break;
								}
							case ActionResult.Success:
								{
									m_txtUninstallFinishedSuccess.Visibility = Visibility.Visible;
									break;
								}
							default:
								{
									if(this.Application.InstallationResult == ActionResult.NotExecuted && e.Restart == ApplyRestart.RestartRequired)
									{
										m_txtUninstallFinishedFailureRestart.Visibility = Visibility.Visible;
									}
									else
									{
										m_txtUninstallFinishedFailure.Visibility = Visibility.Visible;
									}

									break;
								}
						}
						break;
					}
				default:
					{
						switch(this.Application.InstallationResult)
						{
							case ActionResult.UserExit:
								{
									m_txtInstallFinishedCancel.Visibility = Visibility.Visible;
									break;
								}
							case ActionResult.Success:
								{
									if(e.Restart == ApplyRestart.RestartRequired || e.Restart == ApplyRestart.RestartInitiated)
									{
										m_txtInstallFinishedSuccessRestart.Visibility = Visibility.Visible;
									}
									else
									{
										m_txtInstallFinishedSuccess.Visibility = Visibility.Visible;
									}

									break;
								}
							default:
								{
									m_txtInstallFinishedFailure.Visibility = Visibility.Visible;
									break;
								}
						}
						break;
					}
			}
		}
	}
}
