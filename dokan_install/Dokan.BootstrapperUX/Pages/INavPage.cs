using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace Dokan.BootstrapperUX.Pages
{
	public enum Pages
	{
		None = 0,
		InstallationType,
		FeatureSelection,
		PrepateToExecute,
	}

	[Flags]
	public enum NavButtonsUsed
	{
		None = 0,
		Next = 1,
		Install = (1 << 1),
		Restart = (1 << 2),
		Cancel = (1 << 3),
		Close = (1 << 4),
	}

	public interface INavPage
	{
		Pages NextPage { get; }
		NavButtonsUsed Buttons { get; }
		INavPageContainer NavContainer { get; set; }

		void OnPageHiding();
		void OnPageHidden();

		void OnPageShowing();
		void OnPageShown();
	}
}
