using System;
using System.Windows.Forms;
using Tamir.SharpSsh.jsch;

/* PortForwardingR.cs
 * ====================================================================
 * The following example was posted with the original JSch java library,
 * and is translated to C# to show the usage of SharpSSH JSch API
 * ====================================================================
 * */
namespace sharpSshTest.jsch_samples
{
	/// <summary>
	/// This program will demonstrate the port forwarding like option -R of
	/// ssh command; the given port on the remote host will be forwarded to
	/// the given host and port  on the local side.
	/// You will be asked username, hostname, port:host:hostport and passwd. 
	/// If everything works fine, you will get the shell prompt.
	/// Try the port on remote host.
	/// </summary>
	public class PortForwardingR
	{
		public static void RunExample(String[] arg)
		{
			//int port;

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

				//Get from user the remote port, local host and local host port
				String foo = InputForm.GetUserInput("Enter -R port:host:hostport","port:host:hostport");
				int rport=int.Parse(foo.Substring(0, foo.IndexOf(':')));
				foo=foo.Substring(foo.IndexOf(':')+1);
				String lhost=foo.Substring(0, foo.IndexOf(':'));
				int lport=int.Parse(foo.Substring(foo.IndexOf(':')+1));

				// username and password will be given via UserInfo interface.
				UserInfo ui=new MyUserInfo();
				session.setUserInfo(ui);
				session.connect();

				Console.WriteLine(host+":"+rport+" -> "+lhost+":"+lport);

				//Set port forwarding on the opened session
				session.setPortForwardingR(rport, lhost, lport);			
			}
			catch(Exception e)
			{
				Console.WriteLine(e.Message);
			}
		}

		/// <summary>
		/// A user info for getting user data
		/// </summary>
		public class MyUserInfo : UserInfo, UIKeyboardInteractive
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

			public string[] promptKeyboardInteractive(string destination, string name, string instruction, string[] prompt,
			                                          bool[] echo)
			{
				string prmpt = prompt != null && prompt.Length > 0 ? prompt[0] : "";
				passwd=InputForm.GetUserInput(prmpt, true);
				return new string[] { passwd };
			}
		}
	}
}
