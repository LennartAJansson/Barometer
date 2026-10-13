# 1. Öppna en rå anslutning till Redis
$socket = New-Object System.Net.Sockets.TcpClient("redis.ubk3s", 6379)
$stream = $socket.GetStream()
$writer = New-Object System.IO.StreamWriter($stream)
$reader = New-Object System.IO.StreamReader($stream)

# 2. Skicka ditt lösenord (Ersätt med ditt faktiska lösenord om du ändrade det)
$writer.WriteLine("AUTH SuperSecret123!")
$writer.Flush()
$reader.ReadLine()

# 3. Skicka ett PING för att se att databasen svarar
$writer.WriteLine("PING")
$writer.Flush()
$reader.ReadLine()

# 4. Stäng anslutningen
$socket.Close()
