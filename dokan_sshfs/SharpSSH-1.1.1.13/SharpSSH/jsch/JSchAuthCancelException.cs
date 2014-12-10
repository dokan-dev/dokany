using System;

namespace Tamir.SharpSsh.jsch
{
	/// <summary>
	/// Summary description for JSchException.
	/// </summary>
	public class JSchAuthCancelException : JSchException
	{
		public JSchAuthCancelException() : base()
		{
			//
			// TODO: Add constructor logic here
			//
		}

		public JSchAuthCancelException(string msg) : base (msg)
		{
		}
	}
}
