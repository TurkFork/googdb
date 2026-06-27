package main

import (
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"os"
	"strings"
	"sync"
	"time"

	googdb "auth/googdb"

	"golang.org/x/crypto/bcrypt"
)

var (
	db        *googdb.Client
	mu        sync.Mutex
	sessions  = map[string]string{}
)

func main() {
	addr := os.Getenv("GDB_ADDR")
	if addr == "" {
		addr = "127.0.0.1:9606"
	}
	httpAddr := os.Getenv("HTTP_ADDR")
	if httpAddr == "" {
		httpAddr = ":8080"
	}

	var err error
	db, err = googdb.Dial(addr)
	if err != nil {
		log.Fatalf("connect to googdb at %s: %v", addr, err)
	}
	defer db.Close()

	if err := initDB(); err != nil {
		log.Fatalf("init database: %v", err)
	}

	mux := http.NewServeMux()
	mux.HandleFunc("/", handleIndex)
	mux.HandleFunc("/register", handleRegister)
	mux.HandleFunc("/login", handleLogin)
	mux.HandleFunc("/me", handleMe)

	log.Printf("auth service listening on %s (googdb: %s)", httpAddr, addr)
	log.Fatal(http.ListenAndServe(httpAddr, mux))
}

func initDB() error {
	tables, err := db.ListTables()
	if err != nil {
		return fmt.Errorf("list tables: %w", err)
	}
	hasUsers := false
	for _, t := range tables {
		if t == "users" {
			hasUsers = true
			break
		}
	}
	if !hasUsers {
		if err := db.CreateTable("users", []googdb.Column{
			{Name: "id", Type: googdb.GBInt32},
			{Name: "username", Type: googdb.GBString},
			{Name: "password_hash", Type: googdb.GBString},
			{Name: "created_at", Type: googdb.GBInt32},
		}); err != nil {
			return fmt.Errorf("create users table: %w", err)
		}
	}
	return nil
}

func nextID() (int32, error) {
	res, err := db.Select("users")
	if err != nil {
		return 0, err
	}
	var maxID int32
	for _, row := range res.Rows {
		id := row[0].(int32)
		if id > maxID {
			maxID = id
		}
	}
	return maxID + 1, nil
}

// -- JSON types --

type RegisterReq struct {
	Username string `json:"username"`
	Password string `json:"password"`
}

type LoginReq struct {
	Username string `json:"username"`
	Password string `json:"password"`
}

type TokenResp struct {
	Token string `json:"token"`
}

type UserResp struct {
	ID        int32  `json:"id"`
	Username  string `json:"username"`
	CreatedAt int32  `json:"created_at"`
}

type ErrorResp struct {
	Error string `json:"error"`
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	json.NewEncoder(w).Encode(v)
}

func writeError(w http.ResponseWriter, status int, msg string) {
	writeJSON(w, status, ErrorResp{Error: msg})
}

// -- Handlers --

const indexHTML = `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>googdb Auth</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;background:#f5f5f7;display:flex;justify-content:center;padding-top:80px;color:#1d1d1f}
.card{background:#fff;border-radius:16px;padding:40px;width:380px;box-shadow:0 4px 24px rgba(0,0,0,.08)}
h1{font-size:24px;font-weight:600;margin-bottom:24px}
label{display:block;font-size:13px;font-weight:500;margin-bottom:4px;color:#6e6e73}
input{width:100%;padding:10px 12px;border:1px solid #d2d2d7;border-radius:8px;font-size:15px;margin-bottom:16px;outline:none}
input:focus{border-color:#007aff}
.btn{width:100%;padding:10px;border:none;border-radius:8px;font-size:15px;font-weight:500;cursor:pointer;margin-bottom:8px}
.btn-primary{background:#007aff;color:#fff}
.btn-primary:hover{background:#0066d6}
.btn-secondary{background:#e8e8ed;color:#1d1d1f}
.btn-secondary:hover{background:#d2d2d7}
.msg{padding:10px 12px;border-radius:8px;font-size:13px;margin-bottom:16px;display:none}
.msg.error{background:#ffeaea;color:#d32f2f;display:block}
.msg.ok{background:#e8f5e9;color:#2e7d32;display:block}
.tab{display:flex;margin-bottom:24px;gap:8px}
.tab button{flex:1;padding:8px;border:1px solid #d2d2d7;border-radius:8px;background:#fff;cursor:pointer;font-size:13px;font-weight:500}
.tab button.active{background:#007aff;color:#fff;border-color:#007aff}
.form{display:none}
.form.active{display:block}
.info-row{display:flex;justify-content:space-between;padding:12px 0;border-bottom:1px solid #f0f0f2;font-size:15px}
.info-row .label{color:#6e6e73}
.logged-in{display:none}
.logged-in.active{display:block}
</style>
</head>
<body>
<div class="card">
  <h1>googdb Auth</h1>
  <div id="msg" class="msg"></div>
  <div class="tab">
    <button id="tabLogin" class="active" onclick="showTab('login')">Login</button>
    <button id="tabRegister" onclick="showTab('register')">Register</button>
  </div>
  <div id="loginForm" class="form active">
    <label>Username</label><input id="loginUser" type="text" autocomplete="username">
    <label>Password</label><input id="loginPass" type="password" autocomplete="current-password">
    <button class="btn btn-primary" onclick="login()">Login</button>
  </div>
  <div id="registerForm" class="form">
    <label>Username</label><input id="regUser" type="text" autocomplete="username">
    <label>Password</label><input id="regPass" type="password" autocomplete="new-password">
    <button class="btn btn-primary" onclick="register()">Register</button>
  </div>
  <div id="loggedIn" class="logged-in">
    <div class="info-row"><span class="label">ID</span><span id="uid"></span></div>
    <div class="info-row"><span class="label">Username</span><span id="uname"></span></div>
    <div class="info-row"><span class="label">Created</span><span id="ucreated"></span></div>
    <button class="btn btn-secondary" style="margin-top:16px" onclick="logout()">Logout</button>
  </div>
</div>
<script>
function msg(text, type){var m=document.getElementById('msg');m.textContent=text;m.className='msg '+(type||'error')}
function showTab(t){['login','register'].forEach(function(x){document.getElementById(x+'Form').classList.toggle('active',x===t);document.getElementById('tab'+x.charAt(0).toUpperCase()+x.slice(1)).classList.toggle('active',x===t)})}
function login(){var u=document.getElementById('loginUser').value,p=document.getElementById('loginPass').value;msg('','');fetch('/login',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({username:u,password:p})}).then(function(r){return r.json().then(function(d){return{status:r.status,data:d}})}).then(function(r){if(r.status===200){localStorage.setItem('token',r.data.token);showLoggedIn()}else{msg(r.data.error)}})}
function register(){var u=document.getElementById('regUser').value,p=document.getElementById('regPass').value;msg('','');fetch('/register',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({username:u,password:p})}).then(function(r){return r.json().then(function(d){return{status:r.status,data:d}})}).then(function(r){if(r.status===201){msg('Registered! You can now login.','ok')}else{msg(r.data.error)}})}
function showLoggedIn(){var t=localStorage.getItem('token');if(!t)return;fetch('/me',{headers:{'Authorization':'Bearer '+t}}).then(function(r){return r.json().then(function(d){return{status:r.status,data:d}})}).then(function(r){if(r.status===200){document.getElementById('uid').textContent=r.data.id;document.getElementById('uname').textContent=r.data.username;document.getElementById('ucreated').textContent=new Date(r.data.created_at*1000).toLocaleDateString();document.getElementById('loginForm').classList.remove('active');document.getElementById('registerForm').classList.remove('active');document.getElementById('loggedIn').classList.add('active');document.querySelectorAll('.tab button').forEach(function(b){b.style.display='none'})}else{localStorage.removeItem('token')}})}
function logout(){localStorage.removeItem('token');document.getElementById('loggedIn').classList.remove('active');document.getElementById('loginForm').classList.add('active');document.querySelectorAll('.tab button').forEach(function(b){b.style.display=''});document.getElementById('tabLogin').click()}
showLoggedIn();
</script>
</body>
</html>`

func handleIndex(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		http.NotFound(w, r)
		return
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Write([]byte(indexHTML))
}

func handleRegister(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		writeError(w, 405, "POST required")
		return
	}
	var req RegisterReq
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeError(w, 400, "invalid JSON")
		return
	}
	req.Username = strings.TrimSpace(req.Username)
	if req.Username == "" || len(req.Password) < 4 {
		writeError(w, 400, "username required, password min 4 chars")
		return
	}

	// check duplicate
	res, err := db.Select("users")
	if err != nil {
		writeError(w, 500, "db error")
		return
	}
	for _, row := range res.Rows {
		if row[1].(string) == req.Username {
			writeError(w, 409, "username taken")
			return
		}
	}

	hash, err := bcrypt.GenerateFromPassword([]byte(req.Password), bcrypt.DefaultCost)
	if err != nil {
		writeError(w, 500, "internal error")
		return
	}

	id, err := nextID()
	if err != nil {
		writeError(w, 500, "db error")
		return
	}

	now := int32(time.Now().Unix())
	if err := db.Insert("users", []any{id, req.Username, string(hash), now}); err != nil {
		writeError(w, 500, fmt.Sprintf("insert: %v", err))
		return
	}

	writeJSON(w, 201, UserResp{ID: id, Username: req.Username, CreatedAt: now})
}

func handleLogin(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		writeError(w, 405, "POST required")
		return
	}
	var req LoginReq
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeError(w, 400, "invalid JSON")
		return
	}

	res, err := db.SelectWhere("users", "username", req.Username)
	if err != nil {
		writeError(w, 500, "db error")
		return
	}
	if len(res.Rows) == 0 {
		writeError(w, 401, "invalid credentials")
		return
	}

	row := res.Rows[0]
	storedHash := row[2].(string)

	if err := bcrypt.CompareHashAndPassword([]byte(storedHash), []byte(req.Password)); err != nil {
		writeError(w, 401, "invalid credentials")
		return
	}

	token := generateToken()
	mu.Lock()
	sessions[token] = req.Username
	mu.Unlock()

	writeJSON(w, 200, TokenResp{Token: token})
}

func handleMe(w http.ResponseWriter, r *http.Request) {
	if r.Method != "GET" {
		writeError(w, 405, "GET required")
		return
	}
	token := r.Header.Get("Authorization")
	if token == "" {
		writeError(w, 401, "missing Authorization header")
		return
	}
	if strings.HasPrefix(token, "Bearer ") {
		token = token[7:]
	}

	mu.Lock()
	username, ok := sessions[token]
	mu.Unlock()
	if !ok {
		writeError(w, 401, "invalid or expired token")
		return
	}

	res, err := db.SelectWhere("users", "username", username)
	if err != nil || len(res.Rows) == 0 {
		writeError(w, 500, "db error")
		return
	}
	row := res.Rows[0]
	writeJSON(w, 200, UserResp{
		ID:        row[0].(int32),
		Username:  row[1].(string),
		CreatedAt: row[3].(int32),
	})
}

func generateToken() string {
	b := make([]byte, 32)
	rand.Read(b)
	h := sha256.Sum256(b)
	return hex.EncodeToString(h[:])
}
