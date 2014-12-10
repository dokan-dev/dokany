using System;
using Tamir.SharpSsh.jsch;
using System.Collections;
using System.IO;

namespace Tamir.SharpSsh
{
	/// <summary>
	/// Summary description for ExecTest.
	/// </summary>
	public class ExecTest
	{
		public static void Run()
		{
			JSch jsch=new JSch();
			Session session=jsch.getSession("root", "rhmanage", 22);
			session.setPassword( "cisco" );

			Hashtable config=new Hashtable();
			config.Add("StrictHostKeyChecking", "no");
			session.setConfig(config);

			session.connect();
			
				Channel channel=session.openChannel("exec"); 
				((ChannelExec)channel).setCommand("ifconfig");

				StreamReader sr = new StreamReader( channel.getInputStream() );

				channel.connect();

				string line;

				while( (line=sr.ReadLine()) != null )
				{
					Console.WriteLine( line );
				}		

				channel.disconnect();
				session.disconnect();
		}
	}
}
