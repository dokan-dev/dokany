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
	/// Interaction logic for PrepareToExecute.xaml
	/// </summary>
	public partial class PrepareToExecute : UserControl, INavPage, IBootstrapperApplicationAware
	{
		const string BackText = " Click Back to review or change any of your installation settings.";

		DokanBootstrapperApplication m_app;

		NavButtonsUsed INavPage.Buttons
		{
			get { return NavButtonsUsed.Install; }
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

		public DokanBootstrapperApplication Application
		{
			get { return m_app; }
			set
			{
				if(m_app != null)
				{
					m_app.InstallationTypeChanged -= OnInstallationTypeChanged;
				}

				m_app = value;

				if(m_app != null)
				{
					m_app.InstallationTypeChanged += OnInstallationTypeChanged;
				}
			}
		}

		public PrepareToExecute()
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
			if(this.Application != null)
			{
				UpdateInstallationUI(this.Application.InstallationType);
			}
		}

		void INavPage.OnPageShown()
		{
		}

		private void OnInstallationTypeChanged(object sender, InstallationTypeChangedEventArgs e)
		{
			UpdateInstallationUI(e.NewInstallationType);
		}

		public void UpdateInstallationUI(LaunchAction installationType)
		{
			INavPage page = this;

			if(page.NavContainer != null && page.NavContainer.NumberOfPages > 1)
			{
				m_runBack.Text = BackText;
			}
			else
			{
				m_runBack.Text = "";
			}

			switch(installationType)
			{
				case Microsoft.Tools.WindowsInstallerXml.Bootstrapper.LaunchAction.Install:
				case Microsoft.Tools.WindowsInstallerXml.Bootstrapper.LaunchAction.Modify:
					{
						m_runAction.Text = "Install";
						m_runInstallType.Text = "installation";
						break;
					}
				case Microsoft.Tools.WindowsInstallerXml.Bootstrapper.LaunchAction.Repair:
					{
						m_runAction.Text = "Repair";
						m_runInstallType.Text = "installation";
						break;
					}
				case Microsoft.Tools.WindowsInstallerXml.Bootstrapper.LaunchAction.Uninstall:
					{
						m_runAction.Text = "Uninstall";
						m_runInstallType.Text = "uninstallation";
						break;
					}
				case Microsoft.Tools.WindowsInstallerXml.Bootstrapper.LaunchAction.UpdateReplace:
				case Microsoft.Tools.WindowsInstallerXml.Bootstrapper.LaunchAction.UpdateReplaceEmbedded:
					{
						m_runAction.Text = "Repair";
						m_runInstallType.Text = "installation";
						break;
					}
			}
		}
	}
}
