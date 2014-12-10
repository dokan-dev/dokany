using System;
using System.Windows.Forms;

/* StreamForwarding.cs
 * ====================================================================
 * The following example was posted with the original JSch java library,
 * and is translated to C# to show the usage of SharpSSH JSch API
 * ====================================================================
 * */
namespace Tamir.SharpSsh.jsch.examples
{
	/// <summary>
	/// This program will demonstrate the stream forwarding. The given Java
	/// I/O streams will be forwared to the given remote host and port on
	/// the remote side.  It is simmilar to the -L option of ssh command,
	/// but you don't have to assign and open a local tcp port.
	/// You will be asked username, hostname, host:hostport and passwd. 
	/// If everything works fine, System.in and System.out streams will be
	/// forwared to remote port and you can send messages from command line.
	/// </summary>
	public class StreamForwarding
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

				// username and password will be given via UserInfo interface.
				UserInfo ui=new MyUserInfo();
				session.setUserInfo(ui);
				session.connect();

				//Get from user the remote host and remote host port
				String foo = InputForm.GetUserInput("Enter host and port", "host:port");
				host=foo.Substring(0, foo.IndexOf(':'));
				port=int.Parse(foo.Substring(foo.IndexOf(':')+1));

				Console.WriteLine("System.{in,out} will be forwarded to "+
					host+":"+port+".");
				Channel channel=session.openChannel("direct-tcpip");
				((ChannelDirectTCPIP)channel).setInputStream(Console.OpenStandardInput());
				((ChannelDirectTCPIP)channel).setOutputStream(Console.OpenStandardOutput());
				((ChannelDirectTCPIP)channel).setHost(host);
				((ChannelDirectTCPIP)channel).setPort(port);
				channel.connect();

				while(!channel.isClosed())
				{
					System.Threading.Thread.Sleep(500);
				}
				channel.disconnect();
				session.disconnect();
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
