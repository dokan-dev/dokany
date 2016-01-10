using Microsoft.Tools.WindowsInstallerXml.Bootstrapper;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Xml.Linq;

namespace Dokan.BootstrapperUX
{
	public class PackageInstallationInfo
	{
		Dictionary<string, FeatureInstallationInfo> m_features = new Dictionary<string, FeatureInstallationInfo>();
		List<PayloadProperties> m_payloads = new List<PayloadProperties>();
		Version m_existingVersion;
		string m_existingProductCode;
		int m_state;
		int m_isExisting;
		int m_existingPerMachine;
		int m_existingOperation;
		int m_plannedState;

		public PackageProperties Properties
		{
			get;
			private set;
		}

		public PackageState State
		{
			get { return (PackageState)Interlocked.Add(ref m_state, 0); }
			set { Interlocked.Exchange(ref m_state, (int)value); }
		}

		public RequestState RequestedState
		{
			get { return (RequestState)Interlocked.Add(ref m_plannedState, 0); }
			set { Interlocked.Exchange(ref m_plannedState, (int)value); }
		}

		public bool IsExisting
		{
			get { return Interlocked.Add(ref m_isExisting, 0) != 0; }
		}

		public bool ExistingIsPerMachine
		{
			get { return Interlocked.Add(ref m_existingPerMachine, 0) != 0; }
		}

		public string ExistingProductCode
		{
			get
			{
				string prodCode;

				lock (this)
				{
					prodCode = m_existingProductCode;
				}

				return prodCode;
			}
		}

		public Version ExistingVersion
		{
			get
			{
				Version ver;

				lock (this)
				{
					ver = m_existingVersion;
				}

				return ver;
			}
		}

		public RelatedOperation ExistingOperation
		{
			get { return (RelatedOperation)Interlocked.Add(ref m_existingOperation, 0); }
		}

		public IReadOnlyDictionary<string, FeatureInstallationInfo> Features
		{
			get { return m_features; }
		}

		public PackageInstallationInfo(XElement props)
		{
			this.Properties = new PackageProperties(props);
			m_payloads.AddRange(from curElem in props.Document.Descendants(Util.PayloadNamespace) where curElem.Attribute("Package").Value.Equals(this.Properties.Package) select new PayloadProperties(curElem));

			foreach(var feature in (from curElem in props.Document.Descendants(Util.FeatureNamespace) where curElem.Attribute("Package").Value.Equals(this.Properties.Package) select new FeatureInstallationInfo(new FeatureProperties(curElem))))
			{
				m_features[feature.Properties.Feature] = feature;
			}
		}

		public void SetExisting(DetectRelatedMsiPackageEventArgs existingInfo)
		{
			Interlocked.Exchange(ref m_isExisting, 1);
			Interlocked.Exchange(ref m_existingPerMachine, existingInfo.PerMachine ? 1 : 0);
			Interlocked.Exchange(ref m_existingOperation, (int)existingInfo.Operation);

			lock (this)
			{
				m_existingVersion = existingInfo.Version;
				m_existingProductCode = existingInfo.ProductCode;
			}
		}

		public string ToStateString()
		{
			StringBuilder bldr = new StringBuilder(this.Properties.ToStateString());

			bldr.Append("State: ");
			bldr.AppendLine(this.State.ToString());

			bldr.Append("Requested State: ");
			bldr.AppendLine(this.RequestedState.ToString());

			bldr.Append("Is Existing: ");
			bldr.Append(this.IsExisting);
			bldr.AppendLine();

			bldr.Append("Existing Is Per-Machine: ");
			bldr.Append(this.ExistingIsPerMachine);
			bldr.AppendLine();

			bldr.Append("Existing Product Code: ");
			bldr.AppendLine(this.ExistingProductCode ?? "<none>");

			bldr.Append("Existing Version: ");
			bldr.AppendLine(this.ExistingVersion == null ? "<none>" : this.ExistingVersion.ToString());

			bldr.Append("Existing Operation: ");
			bldr.AppendLine(this.ExistingOperation.ToString());

			return bldr.ToString();
		}
	}
}
