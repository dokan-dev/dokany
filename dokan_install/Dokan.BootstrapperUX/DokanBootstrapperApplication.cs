using Microsoft.Deployment.WindowsInstaller;
using Microsoft.Tools.WindowsInstallerXml.Bootstrapper;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Xml.Linq;

namespace Dokan.BootstrapperUX
{
	public class DokanBootstrapperApplication : BootstrapperApplication
	{
		static MainWindow m_mainWindow;
		static object m_mainWindowLock = new object();

		Dictionary<string, PackageInstallationInfo> m_pkgProps = new Dictionary<string, PackageInstallationInfo>();
		MBAPrereqPackage[] m_prereqs;
		BundleProperties m_bundleProps;
		object m_installationTypeChangedLock = new object();
		EventHandler<InstallationTypeChangedEventArgs> m_installationTypeChanged;
		int m_result = (int)ActionResult.NotExecuted;
		int m_returnCode;
		int m_installComplete;
		int m_cancelInstallation;
		int m_installationType;

		public event EventHandler<InstallationTypeChangedEventArgs> InstallationTypeChanged
		{
			add
			{
				lock(m_installationTypeChangedLock)
				{
					m_installationTypeChanged += value;
				}
			}
			remove
			{
				lock(m_installationTypeChangedLock)
				{
					m_installationTypeChanged -= value;
				}
			}
		}

		public bool IsCancelling
		{
			get { return Interlocked.Add(ref m_cancelInstallation, 0) != 0; }
			set { Interlocked.Exchange(ref m_cancelInstallation, value ? 1 : 0); }
		}

		public LaunchAction InstallationType
		{
			get { return (LaunchAction)Interlocked.Add(ref m_installationType, 0); }
			set
			{
				var oldType = this.InstallationType;

				if(oldType != value)
				{
					Interlocked.Exchange(ref m_installationType, (int)value);

					lock (m_installationTypeChangedLock)
					{
						if(m_installationTypeChanged != null)
						{
							var mainWindow = MainWindow;

							if(mainWindow != null)
							{
								mainWindow.Dispatcher.Invoke(new Action(() =>
								{
									try
									{
										m_installationTypeChanged(this, new InstallationTypeChangedEventArgs(oldType, value));
									}
									catch(Exception ex)
									{
										System.Diagnostics.Debug.WriteLine(ex.ToString());
									}
								}));
							}
							else
							{
								try
								{
									m_installationTypeChanged(this, new InstallationTypeChangedEventArgs(oldType, value));
								}
								catch(Exception ex)
								{
									System.Diagnostics.Debug.WriteLine(ex.ToString());
								}
							}
						}
					}
				}
			}
		}

		public int ReturnCode
		{
			get { return Interlocked.Add(ref m_returnCode, 0); }
			set { Interlocked.Exchange(ref m_returnCode, value); }
		}

		public ActionResult InstallationResult
		{
			get { return (ActionResult)Interlocked.Add(ref m_result, 0); }
			set { Interlocked.Exchange(ref m_result, (int)value); }
		}

		public IReadOnlyDictionary<string, PackageInstallationInfo> Packages
		{
			get { return m_pkgProps; }
		}

		public bool IsInstallationComplete
		{
			get { return Interlocked.Add(ref m_installComplete, 0) != 0; }
			private set { Interlocked.Exchange(ref m_installComplete, value ? 1 : 0); }
		}

		public MBAPrereqPackage[] Prerequisites
		{
			get { return m_prereqs; }
		}

		public BundleProperties BundleProperties
		{
			get { return m_bundleProps; }
		}

		public PackageInstallationInfo DokanPackage
		{
			get { return m_pkgProps[Util.DokanPackageName]; }
		}

		public static MainWindow MainWindow
		{
			get
			{
				if(m_mainWindow == null)
				{
					lock (m_mainWindowLock)
					{
						return m_mainWindow;
					}
				}

				return m_mainWindow;
			}

			set
			{
				lock (m_mainWindowLock)
				{
					m_mainWindow = value;
				}
			}
		}

		public DokanBootstrapperApplication()
		{

		}

		protected override void Run()
		{
			this.InstallationType = this.Command.Action;

			try
			{
				Thread curThread = Thread.CurrentThread;

				if(curThread.GetApartmentState() != System.Threading.ApartmentState.STA)
				{
					this.Engine.Log(LogLevel.Standard, "Starting STA thread for UI");

					Thread uiThread = new Thread(UIMain);
					uiThread.SetApartmentState(ApartmentState.STA);
					uiThread.Name = "UI Thread";
					uiThread.Start(this);

					while(uiThread.IsAlive)
					{
						System.Threading.Thread.Sleep(100);
					}
				}
				else
				{
					UIMain(this);
				}

				this.Engine.Quit(this.ReturnCode);
			}
			catch(Exception ex)
			{
				this.ReturnCode = (int)ActionResult.Failure;
				this.Engine.Log(LogLevel.Error, ex.ToString());
				this.Engine.Quit(this.ReturnCode);
			}
		}

		[STAThread]
		static void UIMain(object state)
		{
			DokanBootstrapperApplication installerApp = (DokanBootstrapperApplication)state;

			try
			{
				installerApp.LoadProperties();

				switch(installerApp.Command.Display)
				{
					case Display.Full:
					case Display.Passive:
						{
							Application uiApp = new Application();

							lock (m_mainWindowLock)
							{
								DokanBootstrapperApplication.MainWindow = new MainWindow(installerApp);
							}

							uiApp.Run(DokanBootstrapperApplication.MainWindow);

							break;
						}
					case Display.Embedded:
					case Display.None:
					case Display.Unknown:
						{
							// occurs on other threads from here on out
							installerApp.Engine.Detect();

							while(!installerApp.IsInstallationComplete)
							{
								Thread.Sleep(200);
							}

							break;
						}
				}

				installerApp.ReturnCode = (int)installerApp.InstallationResult;
			}
			catch(Exception ex)
			{
				installerApp.Engine.Log(LogLevel.Error, ex.ToString());
				installerApp.ReturnCode = (int)ActionResult.Failure;
			}
		}

		protected override void OnDetectComplete(DetectCompleteEventArgs args)
		{
			base.OnDetectComplete(args);

			if(DokanBootstrapperApplication.MainWindow == null)
			{
				if(this.InstallationType == LaunchAction.Modify)
				{
					this.InstallationResult = ActionResult.SkipRemainingActions;
					this.IsInstallationComplete = true;
				}
				else
				{
					this.Engine.Plan(this.InstallationType);
				}
			}
		}

		protected override void OnPlanComplete(PlanCompleteEventArgs args)
		{
			base.OnPlanComplete(args);

			if(DokanBootstrapperApplication.MainWindow == null)
			{
				this.Engine.Apply(IntPtr.Zero);
			}
		}

		protected override void OnResolveSource(ResolveSourceEventArgs args)
		{
			if(!File.Exists(args.LocalSource) && !string.IsNullOrWhiteSpace(args.DownloadSource))
			{
				args.Result = Result.Download;
			}

			base.OnResolveSource(args);
		}

		protected override void OnExecutePackageBegin(ExecutePackageBeginEventArgs args)
		{
			if(args.ShouldExecute == false)
			{
				if(this.InstallationResult != ActionResult.UserExit)
				{
					this.InstallationResult = ActionResult.Failure;
				}
			}
			else if(this.InstallationResult == ActionResult.NotExecuted)
			{
				this.InstallationResult = ActionResult.Success;
			}

			base.OnExecutePackageBegin(args);
		}

		protected override void OnApplyComplete(ApplyCompleteEventArgs args)
		{
			try
			{
				base.OnApplyComplete(args);

				if(this.Command.Display == Display.None)
				{
					if(args.Restart != ApplyRestart.None && this.Command.Restart != Restart.Never)
					{
						Util.Restart();
					}
				}
			}
			finally
			{
				this.IsInstallationComplete = true;
			}
		}

		protected override void OnDetectRelatedBundle(DetectRelatedBundleEventArgs args)
		{
			base.OnDetectRelatedBundle(args);
		}

		protected override void OnDetectPackageComplete(DetectPackageCompleteEventArgs args)
		{
			PackageInstallationInfo pkgInfo;

			if(m_pkgProps.TryGetValue(args.PackageId, out pkgInfo))
			{
				pkgInfo.State = args.State;
			}

			base.OnDetectPackageComplete(args);
		}

		protected override void OnDetectMsiFeature(DetectMsiFeatureEventArgs args)
		{
			PackageInstallationInfo pkg;
			FeatureInstallationInfo feature;

			if(this.InstallationType == LaunchAction.Install && args.State != FeatureState.Absent && args.State != FeatureState.Unknown)
			{
				this.InstallationType = LaunchAction.Modify;
			}

			if(m_pkgProps.TryGetValue(args.PackageId, out pkg) && pkg.Features.TryGetValue(args.FeatureId, out feature))
			{
				feature.CurrentState = args.State;
			}
			else
			{
				this.Engine.Log(LogLevel.Error, string.Format("Failed to detect feature ({0}, {1}) because it couldn't be found.", args.FeatureId, args.PackageId));
				args.Result = Result.Error;
			}

			// no features atm
			base.OnDetectMsiFeature(args);
		}

		protected override void OnPlanMsiFeature(PlanMsiFeatureEventArgs args)
		{
			switch(this.InstallationType)
			{
				case LaunchAction.Install:
				case LaunchAction.Modify:
					{
						PackageInstallationInfo pkg;
						FeatureInstallationInfo feature;

						if(m_pkgProps.TryGetValue(args.PackageId, out pkg)
							&& pkg.Features.TryGetValue(args.FeatureId, out feature))
						{
							if(pkg.RequestedState == RequestState.Absent || feature.Properties.Level == 0)
							{
								args.State = FeatureState.Absent;
							}
							else if(feature.RequestedState != FeatureState.Unknown)
							{
								args.State = feature.RequestedState;
							}
							else if(this.Command.Display == Display.None)
							{
								if(feature.Properties.Level == 1 || (this.Engine.NumericVariables.Contains("INSTALLLEVEL") && this.Engine.NumericVariables["INSTALLLEVEL"] >= feature.Properties.Level))
								{
									args.State = FeatureState.Local;
								}
								else
								{
									args.State = FeatureState.Absent;
								}
							}
							else
							{
								this.Engine.Log(LogLevel.Error, string.Format("Failed to plan feature ({0}, {1}) for requested state {2}.", args.FeatureId, args.PackageId, feature.RequestedState));
								args.Result = Result.Error;
								this.IsCancelling = true;
								this.InstallationResult = ActionResult.Failure;
							}
						}
						else
						{
							this.Engine.Log(LogLevel.Error, string.Format("Failed to plan feature ({0}, {1}) because it couldn't be found.", args.FeatureId, args.PackageId));
							args.Result = Result.Error;
							this.IsCancelling = true;
							this.InstallationResult = ActionResult.Failure;
						}

						break;
					}
			}

			base.OnPlanMsiFeature(args);
		}

		protected override void OnDetectRelatedMsiPackage(DetectRelatedMsiPackageEventArgs args)
		{
			PackageInstallationInfo pkgInfo;

			if(m_pkgProps.TryGetValue(args.PackageId, out pkgInfo))
			{
				pkgInfo.SetExisting(args);
			}

			base.OnDetectRelatedMsiPackage(args);
		}

		protected override void OnPlanPackageBegin(PlanPackageBeginEventArgs args)
		{
			PackageInstallationInfo pkgInfo;

			if(m_pkgProps.TryGetValue(args.PackageId, out pkgInfo))
			{
				pkgInfo.RequestedState = args.State;
			}

			this.Engine.Log(LogLevel.Standard, string.Format("Planning package {0} with state {1}", args.PackageId, args.State));

			base.OnPlanPackageBegin(args);
		}

		protected override void OnPlanPackageComplete(PlanPackageCompleteEventArgs args)
		{
			this.Engine.Log(LogLevel.Standard, string.Format("Planned package {0} with state {1}, action {2}, and result {3}", args.PackageId, args.State, args.Execute, args.Status));

			base.OnPlanPackageComplete(args);
		}

		static XDocument GetInstallerApplicationData()
		{
			var asmDir = System.IO.Path.GetDirectoryName(typeof(DokanBootstrapperApplication).Assembly.Location);
			var appDataPath = System.IO.Path.Combine(asmDir, "BootstrapperApplicationData.xml");

			using(StreamReader rdr = new StreamReader(appDataPath))
			{
				return XDocument.Load(rdr);
			}
		}

		void LoadProperties()
		{
			XDocument appData = GetInstallerApplicationData();

#if DEBUG
			string tempFile = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "Dokan_InstallerAppData_" + System.IO.Path.GetFileName(System.IO.Path.ChangeExtension(System.IO.Path.GetTempFileName(), "xml")));
			appData.Save(tempFile, SaveOptions.None);
#endif

			m_bundleProps = new BundleProperties(appData.Root.Element(Util.BundleNamespace));

			m_prereqs = appData.Root.Descendants(Util.PrerequisiteNamespace)
											   .Select(x => new MBAPrereqPackage(x))
											   .ToArray();

			foreach(var prop in from elem in appData.Descendants(Util.PackageNamespace) select new PackageInstallationInfo(elem))
			{
				m_pkgProps.Add(prop.Properties.Package, prop);
			}
		}
	}
}
