#pragma once

const char HTML_HEADER[] = R"(
<!DOCTYPE html>
<html>
  <head>
    <meta charset="utf-8">
    <meta http-equiv="X-UA-Compatible" content="IE=edge">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no" />
    <style>
      body {
        font-family: -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,"Helvetica Neue",Arial,"Noto Sans",sans-serif,"Apple Color Emoji","Segoe UI Emoji","Segoe UI Symbol","Noto Color Emoji";
      }
    </style>
  </head>
  <body>
  )";

const char HTML_SETUP_FORM[] = R"(
    <p>Register a new app in <a href="https://developer.spotify.com/dashboard/">Spotify developer portal</a> 
      and add "http://sprfid.local/callback" as callback URL.</p>
    <p>
      <form action="/save" method="post">
        <label for="clientId">Client ID:</label> <input name="clientId" id="clientId" type="text" size="32" value="%s"><br>
        <label for="clientSecret">Client Secret:</label> <input name="clientSecret" id="clientSecret" type="password" size="32"><br>
        <input type="submit" value="save">
      </form>
    </p></body></html>
)";