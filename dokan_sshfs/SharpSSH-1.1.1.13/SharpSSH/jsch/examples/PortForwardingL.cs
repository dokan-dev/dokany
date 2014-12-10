using System;
using System.Windows.Forms;

/* PortForwardingL.cs
 * ====================================================================
 * The following example was posted with the original JSch java library,
 * and is translated to C# to show the usage of SharpSSH JSch API
 * ====================================================================
 * */
namespace Tamir.SharpSsh.jsch.examples
{
	/// <summary>
	/// This program will demonstrate the port forwarding like option -L of
	/// ssh command; the given port on the local host will be forwarded to
	/// the given remote host and port on the remote side.
	/// You will be asked username, hostname, port:host:hostport and passwd. 
	/// If everything works fine, you will get the shell prompt.
	/// Try the port on localhost.
	/// </summary>
	public class PortForwardingL
	{
		public static void Main(String[] arg)
		{
			int port;

			try
			{
				//Create a new JSch instance
				JSch jsch=new JSch();

				//Prompt for username and server host
				Console.WriteLine("Please enter the user and host info at the popup window...");
				String host = InputForm.GetUserInput
					("Enter username@hostname",
					Environment.UserName+"@localhost");
				String user=host.Substring(0, host.IndexOf('@'));
				host=host.Substring(host.IndexOf('@')+1);

				//Create a new SSH session
				Session session=jsch.getSession(user, host, 22);

				//Get from user the local port, remote host and remote host port
				String foo = InputForm.GetUserInput("Enter -L port:host:hostport","port:host:hostport");
				int lport=int.Parse(foo.Substring(0, foo.IndexOf(':')));
				foo=foo.Substring(foo.IndexOf(':')+1);
				String rhost=foo.Substring(0, foo.IndexOf(':'));
				int rport=int.Parse(foo.Substring(foo.IndexOf(':')+1));

				// username and password will be given via UserInfo interface.
				UserInfo ui=new MyUserInfo();
				session.setUserInfo(ui);
				session.connect();

				Console.WriteLine("localhost:"+lport+" -> "+rhost+":"+rport);

				//Set port forwarding on the opened session
				session.setPortForwardingL(lport, rhost, rport);			
			}
			catch(Exception e)
			{
				Console.WriteLine(e);
			}
		}

		/// <summary>
		/// A user info for getting user data
		/// </summary>
		public class MyUserInfo : UserInfo
		{
			/// <summary>
			/// Holds the user password
			/// </summary>
			private String passwd;

			/// <summary>
			/// Returns the user password
			/// </summary>
			public String getPassword(){ return passwd; }

			/// <summary>
			/// Prompt the user for a Yes/No input
			/// </summary>
			public bool promptYesNo(String str)
			{
				return InputForm.PromptYesNo(str);
			}
			
			/// <summary>
			/// Returns the user passphrase (passwd for the private key file)
			/// </summary>
			public String getPassphrase(){ return null; }

			/// <summary>
			/// Prompt the user for a passphrase (passwd for the private key file)
			/// </summary>
			public bool promptPassphrase(String message){ return true; }

			/// <summary>
			/// Prompt the user for a password
			/// </summary>\
			public bool promptPassword(String message)
			{
				passwd=InputForm.GetUserInput(message, true);
				return true;
			}

			/// <summary>
			/// Shows a message to the user
			/// </summary>
			public void showMessage(String message)
			{
				InputForm.ShowMessage(message);
			}
		}
	}
}
