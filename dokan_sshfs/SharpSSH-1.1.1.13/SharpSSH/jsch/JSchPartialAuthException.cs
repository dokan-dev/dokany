using System;

namespace Tamir.SharpSsh.jsch
{
	/// <summary>
	/// Summary description for JSchException.
	/// </summary>
	public class JSchPartialAuthException : JSchException
	{
		string methods;
		public JSchPartialAuthException() : base()
		{
			methods = null;
		}

		public JSchPartialAuthException(string msg) : base (msg)
		{
			methods = msg;
		}

		public String getMethods()
		{
			return methods;
		}
	}
}
