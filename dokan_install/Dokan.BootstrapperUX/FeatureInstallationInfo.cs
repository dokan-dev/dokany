using Microsoft.Tools.WindowsInstallerXml.Bootstrapper;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace Dokan.BootstrapperUX
{
	public class FeatureInstallationInfo
	{
		int m_currentState;
		int m_requestedState;

		public FeatureProperties Properties
		{
			get;
			private set;
		}

		public FeatureState CurrentState
		{
			get { return (FeatureState)Interlocked.Add(ref m_currentState, 0); }
			set { Interlocked.Exchange(ref m_currentState, (int)value); }
		}

		public FeatureState RequestedState
		{
			get { return (FeatureState)Interlocked.Add(ref m_requestedState, 0); }
			set { Interlocked.Exchange(ref m_requestedState, (int)value); }
		}

		public FeatureInstallationInfo(FeatureProperties properties)
		{
			this.Properties = properties;
		}
	}
}
