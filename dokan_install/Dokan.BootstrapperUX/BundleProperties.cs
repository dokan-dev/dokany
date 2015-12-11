using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Xml.Linq;

namespace Dokan.BootstrapperUX
{
	public class BundleProperties : WixElemBase
	{
		public string DisplayName
		{
			get;
			private set;
		}

		public string LogPathVariable
		{
			get;
			private set;
		}

		public bool Compressed
		{
			get;
			private set;
		}

		public bool PerMachine
		{
			get;
			private set;
		}

		public Guid Id
		{
			get;
			private set;
		}

		public Guid UpgradeCode
		{
			get;
			private set;
		}

		public BundleProperties(XElement elem)
		{
			this.DisplayName = GetString(elem, "DisplayName");
			this.LogPathVariable = GetString(elem, "LogPathVariable");
			this.Compressed = GetYesNo(elem, "Compressed");
			this.PerMachine = GetYesNo(elem, "PerMachine");
			this.Id = GetGuid(elem, "Id");
			this.UpgradeCode = GetGuid(elem, "UpgradeCode");
		}

		public string ToStateString()
		{
			StringBuilder bldr = new StringBuilder();

			bldr.Append("Display Name: ");
			bldr.AppendLine(this.DisplayName);

			bldr.Append("Log Path Variable: ");
			bldr.AppendLine(this.LogPathVariable);

			bldr.Append("Compressed: ");
			bldr.AppendLine(this.Compressed.ToString());

			bldr.Append("Per-Machine: ");
			bldr.AppendLine(this.PerMachine.ToString());

			bldr.Append("Id: ");
			bldr.AppendLine(this.Id.ToString());

			bldr.Append("Upgrade Code: ");
			bldr.AppendLine(this.UpgradeCode.ToString());

			return bldr.ToString();
		}
	}
}
