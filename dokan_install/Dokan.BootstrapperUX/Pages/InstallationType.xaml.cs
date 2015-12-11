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
	/// Interaction logic for InstallationType.xaml
	/// </summary>
	public partial class InstallationType : UserControl, INavPage
	{
		NavButtonsUsed INavPage.Buttons
		{
			get { return NavButtonsUsed.None; }
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

		public InstallationType()
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
	}
}
