The following examples were posted with the original JSch java library,
and were translated to C#
====================================================================

- Shell.cs
  This program enables you to connect to sshd server and get the shell prompt.
  You will be asked username, hostname and passwd. 
  If everything works fine, you will get the shell prompt. Output will
  be ugly because of lacks of terminal-emulation, but you can issue commands.
  
- Exec.cs
  This program will demonstrate remote exec.
  You will be asked username, hostname, displayname, passwd and command.
  If everything works fine, given command will be invoked 
  on the remote side and outputs will be printed out. In this sample,
  X forwarding is enabled, so you can give an X client as a command.

- PortForwardingR.cs
  This program will demonstrate the port forwarding like option -R of
  ssh command; the given port on the remote host will be forwarded to
  the given host and port  on the local side.
  You will be asked username, hostname, port:host:hostport and passwd. 
  If everything works fine, you will get the shell prompt.
  Try the port on remote host.

- PortForwardingL.cs
  This program will demonstrate the port forwarding like option -L of
  ssh command; the given port on the local host will be forwarded to
  the given remote host and port on the remote side.
  You will be asked username, hostname, port:host:hostport and passwd. 
  If everything works fine, you will get the shell prompt.
  Try the port on localhost.

- StreamForwarding.cs
  This program will demonstrate the stream forwarding. The given Java
  I/O streams will be forwared to the given remote host and port on
  the remote side.  It is simmilar to the -L option of ssh command,
  but you don't have to assign and open a local tcp port.
  You will be asked username, hostname, host:hostport and passwd. 
  If everything works fine, System.in and System.out streams will be
  forwared to remote port and you can send messages from command line.

- UserAuthPubKey.cs
  This program will demonstrate the user authentification by public key.
  You will be asked username, hostname, privatekey(id_dsa) and passphrase. 
  If everything works fine, you will get the shell prompt

- ScpTo.cs
  This program will demonstrate the file transfer from local to remote.
  You will be asked passwd. 
  If everything works fine, a local file 'file1' will copied to
  'file2' on 'remotehost'.

- ScpFrom.cs
  This program will demonstrate the file transfer from remote to local
  You will be asked passwd. 
  If everything works fine, a file 'file1' on 'remotehost' will copied to
  local 'file1'.

- Sftp.cs
  This program will demonstrate the sftp protocol support.
  You will be asked username, host and passwd. 
  If everything works fine, you will get a prompt 'sftp>'. 
  'help' command will show available command.
  In current implementation, the destination path for 'get' and 'put'
  commands must be a file, not a directory.

- KnownHosts.cs
  This program will demonstrate the 'known_hosts' file handling.
  You will be asked username, hostname, a path for 'known_hosts' and passwd. 
  If everything works fine, you will get the shell prompt.
  In current implementation, jsch only reads 'known_hosts' for checking
  and does not modify it.

- KeyGen.cs
  This progam will demonstrate the RSA/DSA keypair generation. 
  You will be asked a passphrase for output_keyfile.
  If everything works fine, you will get the DSA or RSA keypair, 
  output_keyfile and output_keyfile+".pub".
  The private key and public key are in the OpenSSH format.

- ChangePassphrase.cs
  This program will demonstrate to change the passphrase for a
  private key file instead of creating a new private key.
  A passphrase will be prompted if the given private-key has been
  encrypted.  After successfully loading the content of the
  private-key, the new passphrase will be prompted and the given
  private-key will be re-encrypted with that new passphrase.
  
- AES.cs
  This program will demonstrate how to use "aes128-cbc".

- Daemon.cs
  This program will demonstrate how to provide a network service like
  inetd by using remote port-forwarding functionality.
  
- ViaHTTP.cs
  This program will demonstrate the ssh session via HTTP proxy.
  You will be asked username, hostname, proxy-server and passwd. 
  If everything works fine, you will get the shell prompt.
