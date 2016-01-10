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
	/// Interaction logic for FeatureSelection.xaml
	/// </summary>
	public partial class FeatureSelection : UserControl, INavPage, IBootstrapperApplicationAware
	{
		class FeatureNode
		{
			public FeatureInstallationInfo Feature { get; private set; }
			public List<FeatureNode> Children { get; private set; }

			public FeatureNode(FeatureInstallationInfo props)
			{
				this.Feature = props;
				this.Children = new List<FeatureNode>();
			}
		}

		IReadOnlyList<FeatureItem> m_rootFeatures;

		NavButtonsUsed INavPage.Buttons
		{
			get { return NavButtonsUsed.Next; }
		}

		Pages INavPage.NextPage
		{
			get { return Pages.PrepateToExecute; }
		}

		INavPageContainer INavPage.NavContainer
		{
			get;
			set;
		}

		public DokanBootstrapperApplication Application
		{
			get;
			set;
		}

		public FeatureSelection()
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
			UpdateText();
		}

		void INavPage.OnPageShown()
		{
		}

		void UpdateText()
		{
			if(this.Application != null)
			{
				if(this.Application.InstallationType == Microsoft.Tools.WindowsInstallerXml.Bootstrapper.LaunchAction.Modify)
				{
					m_txtModify.Visibility = Visibility.Visible;
					m_txtInstall.Visibility = Visibility.Collapsed;
				}
				else
				{
					m_txtModify.Visibility = Visibility.Collapsed;
					m_txtInstall.Visibility = Visibility.Visible;
				}
			}
		}

		public void LoadFeatures()
		{
			if(this.Application == null)
			{
				return;
			}

			UpdateText();

			// We build the UI tree from the leaf nodes up
			Dictionary<string, FeatureNode> hierarchy = new Dictionary<string, FeatureNode>();

			foreach(var feature in this.Application.DokanPackage.Features.Values)
			{
				System.Diagnostics.Debug.Assert(!hierarchy.ContainsKey(feature.Properties.Feature));

				FeatureNode newNode = new FeatureNode(feature);

				hierarchy[feature.Properties.Feature] = newNode;
			}

			foreach(var pair in hierarchy)
			{
				FeatureNode parentNode;

				if(!string.IsNullOrWhiteSpace(pair.Value.Feature.Properties.Parent) && hierarchy.TryGetValue(pair.Value.Feature.Properties.Parent, out parentNode))
				{
					parentNode.Children.Add(pair.Value);
				}
			}

			List<FeatureItem> rootFeatures = new List<FeatureItem>();
			m_rootFeatures = rootFeatures;

			foreach(var pair in hierarchy)
			{
				if(string.IsNullOrWhiteSpace(pair.Value.Feature.Properties.Parent))
				{
					rootFeatures.Add(GenerateItemHierarchy(pair.Value));
				}
			}

			m_treeView.ItemsSource = rootFeatures;

			if((this.Application.InstallationType == Microsoft.Tools.WindowsInstallerXml.Bootstrapper.LaunchAction.Install)
				&& this.Application.Engine.NumericVariables.Contains("INSTALLLEVEL"))
			{
				System.Windows.MessageBox.Show("HI!");
				var level = this.Application.Engine.NumericVariables["INSTALLLEVEL"];

				LoadLevel((int)level);
			}
		}

		public void LoadLevel(int level)
		{
			foreach(var node in m_rootFeatures)
			{
				node.LoadLevel(level);
			}
		}

		static FeatureItem GenerateItemHierarchy(FeatureNode node)
		{
			if(node.Children.Count > 0)
			{
				List<FeatureItem> childItems = new List<FeatureItem>(node.Children.Count);

				foreach(var child in node.Children)
				{
					childItems.Add(GenerateItemHierarchy(child));
				}

				return new FeatureItem(node.Feature, childItems.ToArray());
			}

			return new FeatureItem(node.Feature);
		}
	}
}
