using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Xml.Linq;

namespace Dokan.BootstrapperUX
{
	public class PayloadProperties : WixElemBase, IPackageItem
	{
		public string Payload
		{
			get;
			private set;
		}

		public string Package
		{
			get;
			private set;
		}

		public string Name
		{
			get;
			private set;
		}

		public string Container
		{
			get;
			private set;
		}

		public int Size
		{
			get;
			private set;
		}

		public string DownloadUrl
		{
			get;
			private set;
		}

		public bool LayoutOnly
		{
			get;
			private set;
		}

		public PayloadProperties(XElement elem)
		{
			this.Payload = GetString(elem, "Payload");
			this.Package = GetString(elem, "Package");
			this.Name = GetString(elem, "Name");
			this.Container = GetString(elem, "Container");
			this.Size = GetInt(elem, "Size");
			this.DownloadUrl = GetString(elem, "DownloadUrl");
			this.LayoutOnly = GetYesNo(elem, "LayoutOnly");
		}

		public string ToStateString()
		{
			StringBuilder bldr = new StringBuilder();

			bldr.Append("Payload: ");
			bldr.AppendLine(this.Payload ?? "<none>");

			bldr.Append("Package: ");
			bldr.AppendLine(this.Package ?? "<none>");

			bldr.Append("Name: ");
			bldr.AppendLine(this.Name ?? "<none>");

			bldr.Append("Container: ");
			bldr.AppendLine(this.Container ?? "<none>");

			bldr.Append("Size: ");
			bldr.Append(this.Size);
			bldr.AppendLine();

			bldr.Append("Download Url: ");
			bldr.AppendLine(this.DownloadUrl ?? "<none>");

			bldr.Append("Layout Only: ");
			bldr.Append(this.LayoutOnly);
			bldr.AppendLine();

			return bldr.ToString();
		}
	}
}
