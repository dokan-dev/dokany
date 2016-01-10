using Microsoft.Deployment.WindowsInstaller;
using Microsoft.Tools.WindowsInstallerXml.Bootstrapper;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Documents;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Shapes;
using System.ComponentModel;

namespace Dokan.BootstrapperUX
{
	/// <summary>
	/// Interaction logic for MainWindow.xaml
	/// </summary>
	public partial class MainWindow : Window, Pages.INavPageContainer
	{
		static readonly TimeSpan DefaultTransitionTime = TimeSpan.FromMilliseconds(10);

		DokanBootstrapperApplication m_app;
		LinkedList<UserControl> m_navStack = new LinkedList<UserControl>();
		bool m_isDraggingWindow;
		Point m_curMousePos;
		bool m_isInstalling;

		int Pages.INavPageContainer.NumberOfPages
		{
			get { return m_navStack.Count; }
		}

		public MainWindow()
		{
			InitializeComponent();

			foreach(var page in m_gridPages.Children)
			{
				if(page is Pages.INavPage)
				{
					((Pages.INavPage)page).NavContainer = this;
				}
			}
		}

		public MainWindow(DokanBootstrapperApplication app)
			: this()
		{
			//MessageBox.Show("Attach debugger");

			m_app = app;

			m_txtVersion.Text = app.DokanPackage.Properties.Version.ToString();

			m_pageProgress.Application = app;
			m_pageComplete.Application = app;
			m_pagePrepareToExecute.Application = app;
			m_pageFeatureSelection.Application = app;

			this.Loaded += MainWindow_Loaded;

			app.DetectComplete += Installer_DetectComplete;
			app.PlanComplete += Installer_PlanComplete;
			app.ApplyComplete += Installer_ApplyComplete;
			app.ExecutePackageBegin += Installer_ExecutePackageBegin;
			app.ExecuteProgress += Installer_ExecuteProgress;
			app.CacheAcquireBegin += Installer_CacheAcquireBegin;
			app.CacheAcquireProgress += Installer_CacheAcquireProgress;
			app.InstallationTypeChanged += App_InstallationTypeChanged;

			UpdateExecuteButton(m_app.InstallationType);
		}

		private void App_InstallationTypeChanged(object sender, InstallationTypeChangedEventArgs e)
		{
			UpdateExecuteButton(e.NewInstallationType);
		}

		void UpdateExecuteButton(LaunchAction installationType)
		{
			switch(installationType)
			{
				case LaunchAction.Install:
					{
						m_btnInstall.Content = "Install";
						m_btnInstall.Command = InstallerCommands.Install;
						break;
					}
				case LaunchAction.Modify:
					{
						m_btnInstall.Content = "Install";
						m_btnInstall.Command = InstallerCommands.Modify;
						break;
					}
				case LaunchAction.Repair:
					{
						m_btnInstall.Content = "Repair";
						m_btnInstall.Command = InstallerCommands.Repair;
						break;
					}
				case LaunchAction.Uninstall:
					{
						m_btnInstall.Content = "Uninstall";
						m_btnInstall.Command = InstallerCommands.Uninstall;
						break;
					}
				case LaunchAction.UpdateReplace:
				case LaunchAction.UpdateReplaceEmbedded:
					{
						m_btnInstall.Content = "Update";
						m_btnInstall.Command = InstallerCommands.Install;
						break;
					}
			}

			m_pagePrepareToExecute.UpdateInstallationUI(installationType);
		}

		void Installer_CacheAcquireProgress(object sender, CacheAcquireProgressEventArgs e)
		{
			if(m_app.IsCancelling)
			{
				e.Result = Result.Cancel;
			}
		}

		void Installer_CacheAcquireBegin(object sender, CacheAcquireBeginEventArgs e)
		{
			if(m_app.IsCancelling)
			{
				e.Result = Result.Cancel;
			}
		}

		void Installer_ExecuteProgress(object sender, ExecuteProgressEventArgs e)
		{
			if(m_app.IsCancelling)
			{
				e.Result = Result.Cancel;
			}
		}

		void Installer_ExecutePackageBegin(object sender, ExecutePackageBeginEventArgs e)
		{
			if(m_app.IsCancelling)
			{
				e.Result = Result.Cancel;
			}
		}

		void Installer_ApplyComplete(object sender, ApplyCompleteEventArgs e)
		{
			this.Dispatcher.BeginInvoke(new Action(() =>
			{
				m_isInstalling = false;
				m_app.IsCancelling = false;

				if(e.Restart != ApplyRestart.None && (m_app.Command.Restart == Restart.Always || m_app.Command.Restart == Restart.Automatic))
				{
					Util.Restart();
					this.Close();
				}

				if(m_app.Command.Display == Display.Passive)
				{
					if(e.Restart != ApplyRestart.None && m_app.Command.Restart != Restart.Never)
					{
						Util.Restart();
					}
					
					this.Close();
				}
				else
				{
					m_pageComplete.LoadState(e);
					PopAllPushPage(m_pageComplete);
				}
			}));
		}

		void MainWindow_Loaded(object sender, RoutedEventArgs e)
		{
			m_app.Engine.Detect(new WindowInteropHelper(this).Handle);
		}

		void Installer_PlanComplete(object sender, PlanCompleteEventArgs e)
		{
			this.Dispatcher.BeginInvoke(new Action(() =>
			{
				m_isInstalling = true;
				m_app.Engine.Apply(new WindowInteropHelper(this).Handle);
			}));
		}

		void Installer_DetectComplete(object sender, DetectCompleteEventArgs e)
		{
			this.Dispatcher.BeginInvoke(new Action(() =>
			{
				m_pageFeatureSelection.LoadFeatures();

				if(m_app.Command.Display == Display.Passive)
				{
					if(m_app.InstallationType == LaunchAction.Install)
					{
						m_pageFeatureSelection.LoadLevel(InstallationLevels.Typical);
					}

					m_app.Engine.Plan(m_app.InstallationType);
					PushPage(m_pageProgress);
				}
				else
				{
					switch(m_app.InstallationType)
					{
						case LaunchAction.Install:
							{
								m_pageFeatureSelection.LoadLevel(InstallationLevels.Typical);
								PushPage(m_pageInstallationType);
								break;
							}
						case LaunchAction.Modify:
							{
								PushPage(m_pageFeatureSelection);
								break;
							}
						case LaunchAction.Repair:
							{
								PushPage(m_pagePrepareToExecute);
								break;
							}
						case LaunchAction.UpdateReplace:
						case LaunchAction.UpdateReplaceEmbedded:
							{
#warning TODO: UpdateReplace
								break;
							}
						case LaunchAction.Uninstall:
							{
								PushPage(m_pagePrepareToExecute);
								break;
							}
					}
				}
			}));
		}

		private void CommandBinding_InstallTypical_Executed(object sender, ExecutedRoutedEventArgs e)
		{
			m_pageFeatureSelection.LoadLevel(InstallationLevels.Typical);
			PushPage(m_pagePrepareToExecute);
		}

		private void CommandBinding_InstallComplete_Executed(object sender, ExecutedRoutedEventArgs e)
		{
			m_pageFeatureSelection.LoadLevel(InstallationLevels.Complete);
			PushPage(m_pagePrepareToExecute);
		}

		private void CommandBinding_ChooseFeatures_Executed(object sender, ExecutedRoutedEventArgs e)
		{
			PushPage(m_pageFeatureSelection);
		}

		void PushPage(UserControl page, TransitionCompletedDelegate completed = null)
		{
			if(page == null)
			{
				throw new ArgumentNullException("page");
			}

			UIElement curPage = null;

			if(m_navStack.Count > 0)
			{
				curPage = m_navStack.Last.Value;
			}
			else if(m_progressRing.Visibility != Visibility.Collapsed)
			{
				curPage = m_progressRing;
			}

			m_navStack.AddLast(page);

			ExecutePageTransition(curPage, page, completed);
		}

		void PopAllPushPage(UserControl page, TransitionCompletedDelegate completed = null)
		{
			var curPage = m_navStack.Last.Value;
			m_navStack.Clear();
			m_navStack.AddLast(page);

			ExecutePageTransition(curPage, page, completed);
		}

		private void CommandBinding_Back_Executed(object sender, ExecutedRoutedEventArgs e)
		{
			if(m_navStack.Count <= 1)
			{
				return;
			}

			var currentPage = m_navStack.Last.Value;
			m_navStack.RemoveLast();

			ExecutePageTransition(currentPage, m_navStack.Last.Value);
		}

		void ExecutePageTransition(UIElement fadeOut, UIElement fadeIn, TransitionCompletedDelegate completed = null)
		{
			TriggerPageBeginTransitionEvents(fadeOut as Pages.INavPage, fadeIn as Pages.INavPage);

			BuildPageTransition(fadeOut, fadeIn, DefaultTransitionTime).Start(() =>
			{
				TriggerPageEndTransitionEvents(fadeOut as Pages.INavPage, fadeIn as Pages.INavPage);

				if(completed != null)
				{
					try
					{
						completed();
					}
					catch(Exception ex)
					{
						System.Diagnostics.Debug.WriteLine(ex.ToString());
					}
				}
			});
		}

		void TriggerPageBeginTransitionEvents(Pages.INavPage fadeOut, Pages.INavPage fadeIn)
		{
			if(fadeOut != null)
			{
				try
				{
					fadeOut.OnPageHiding();
				}
				catch(Exception ex)
				{
					System.Diagnostics.Debug.WriteLine(ex.ToString());
				}
			}

			if(fadeIn != null)
			{
				try
				{
					fadeIn.OnPageShowing();
				}
				catch(Exception ex)
				{
					System.Diagnostics.Debug.WriteLine(ex.ToString());
				}
			}
		}

		void TriggerPageEndTransitionEvents(Pages.INavPage fadeOut, Pages.INavPage fadeIn)
		{
			if(fadeOut != null)
			{
				try
				{
					fadeOut.OnPageHidden();
				}
				catch(Exception ex)
				{
					System.Diagnostics.Debug.WriteLine(ex.ToString());
				}
			}

			if(fadeIn != null)
			{
				try
				{
					fadeIn.OnPageShown();
				}
				catch(Exception ex)
				{
					System.Diagnostics.Debug.WriteLine(ex.ToString());
				}
			}
		}

		private void CommandBinding_Close_Executed(object sender, ExecutedRoutedEventArgs e)
		{
			this.Close();
		}

		private void CommandBinding_Restart_Executed(object sender, ExecutedRoutedEventArgs e)
		{
			Util.Restart();

			this.Close();
		}

		FadeOutThenInTransition BuildPageTransition(UIElement fadeOut, UIElement fadeIn, TimeSpan transitionTime)
		{
			Pages.PageFadeOutInBuilder bldr = new Pages.PageFadeOutInBuilder(fadeOut, fadeIn);

			if(m_navStack.Count == 1 && m_btnBack.Visibility != Visibility.Collapsed)
			{
				bldr.AddToFadeOut(m_btnBack);
			}
			else if(m_navStack.Count > 1 && m_btnBack.Visibility == Visibility.Collapsed)
			{
				bldr.AddToFadeIn(m_btnBack);
			}

			bldr.AddButton(m_btnInstall, Pages.NavButtonsUsed.Install);
			bldr.AddButton(m_btnNext, Pages.NavButtonsUsed.Next);
			bldr.AddButton(m_btnRestart, Pages.NavButtonsUsed.Restart);
			bldr.AddButton(m_btnCancel, Pages.NavButtonsUsed.Cancel);
			bldr.AddButton(m_btnClose, Pages.NavButtonsUsed.Close);

			Pages.INavPage nextPage = fadeIn as Pages.INavPage;

			if(nextPage != null && (nextPage.Buttons & Pages.NavButtonsUsed.Restart) == Pages.NavButtonsUsed.Restart && nextPage.Buttons != Pages.NavButtonsUsed.Restart)
			{
				m_btnRestart.Margin = new Thickness(0, 0, 5, 0);
			}
			else
			{
				m_btnRestart.Margin = new Thickness(0);
			}

			return bldr.BuildTransition(transitionTime);
		}

		private void CommandBinding_Next_Executed(object sender, ExecutedRoutedEventArgs e)
		{
			var curPage = m_navStack.Last.Value as Pages.INavPage;

			if(curPage != null)
			{
				switch(curPage.NextPage)
				{
					case Pages.Pages.PrepateToExecute:
						{
							PushPage(m_pagePrepareToExecute);
							break;
						}
				}
			}
		}

		private void CommandBinding_ExecutePlan_Executed(object sender, ExecutedRoutedEventArgs e)
		{
			PopAllPushPage(m_pageProgress, () =>
			{
				m_app.Engine.Plan(m_app.InstallationType);
			});
		}

		private void CommandBinding_Cancel_Executed(object sender, ExecutedRoutedEventArgs e)
		{
			CancelInstallation();
		}

		protected override void OnMouseDown(MouseButtonEventArgs e)
		{
			base.OnMouseDown(e);

			if(!e.Handled && e.ChangedButton == MouseButton.Left)
			{
				m_curMousePos = e.GetPosition(this);

				m_isDraggingWindow = true;
				e.Handled = true;

				this.CaptureMouse();
			}
		}

		protected override void OnMouseMove(MouseEventArgs e)
		{
			base.OnMouseMove(e);

			if(m_isDraggingWindow)
			{
				Point newPos = e.GetPosition(this);
				Vector diff = newPos - m_curMousePos;

				this.Left += diff.X;
				this.Top += diff.Y;

				e.Handled = true;
			}
		}

		protected override void OnMouseUp(MouseButtonEventArgs e)
		{
			base.OnMouseUp(e);

			if(m_isDraggingWindow)
			{
				m_isDraggingWindow = false;
				e.Handled = true;

				this.ReleaseMouseCapture();
			}
		}

		protected override void OnLostFocus(RoutedEventArgs e)
		{
			base.OnLostFocus(e);

			if(m_isDraggingWindow)
			{
				m_isDraggingWindow = false;
				e.Handled = true;

				this.ReleaseMouseCapture();
			}
		}

		protected override void OnLostMouseCapture(MouseEventArgs e)
		{
			base.OnLostMouseCapture(e);

			m_isDraggingWindow = false;
		}

		protected override void OnClosing(CancelEventArgs e)
		{
			if(m_isInstalling)
			{
				e.Cancel = true;

				CancelInstallation();
			}
			else
			{
				base.OnClosing(e);
			}
		}

		void CancelInstallation()
		{
			if(m_isInstalling && !m_app.IsCancelling)
			{
				var result = System.Windows.MessageBox.Show(this, "Are you sure you want to cancel?", "Warning", MessageBoxButton.YesNo, MessageBoxImage.Warning);

				if(result == MessageBoxResult.Yes)
				{
					NotifyCancelInstallation();
				}
			}
		}

		private void NotifyCancelInstallation()
		{
			if(m_app.InstallationResult != ActionResult.UserExit && m_app.Command.Display != Display.Passive)
			{
				m_app.Engine.Log(LogLevel.Standard, "User canceled installation!");

				if(m_app.InstallationResult != ActionResult.Failure)
				{
					m_app.InstallationResult = ActionResult.UserExit;
				}

				m_app.IsCancelling = true;
			}
		}
	}
}
