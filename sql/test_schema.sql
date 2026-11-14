CREATE DATABASE IF NOT EXISTS bank_test;
USE bank_test;

CREATE TABLE customers (
    customer_id INT AUTO_INCREMENT PRIMARY KEY,
    name        VARCHAR(100) NOT NULL,
    email       VARCHAR(120) NOT NULL UNIQUE,
    created_at  TIMESTAMP DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB;

CREATE TABLE accounts (
    account_id  INT AUTO_INCREMENT PRIMARY KEY,
    customer_id INT NOT NULL,
    balance     DECIMAL(15,2) NOT NULL DEFAULT 0.00,
    status      ENUM('ACTIVE','FROZEN','CLOSED') NOT NULL DEFAULT 'ACTIVE',
    version     INT NOT NULL DEFAULT 0,
    updated_at  TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    CONSTRAINT fk_acc_cust FOREIGN KEY (customer_id) REFERENCES customers(customer_id),
    CONSTRAINT chk_balance CHECK (balance >= 0)
) ENGINE=InnoDB;

CREATE TABLE transactions (
    txn_id       BIGINT AUTO_INCREMENT PRIMARY KEY,
    request_id   CHAR(36) NOT NULL UNIQUE,
    type         ENUM('DEPOSIT','WITHDRAW','TRANSFER') NOT NULL,
    from_account INT NULL,
    to_account   INT NULL,
    amount       DECIMAL(15,2) NOT NULL CHECK (amount > 0),
    status       ENUM('COMMITTED','ROLLED_BACK') NOT NULL,
    created_at   TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (from_account) REFERENCES accounts(account_id),
    FOREIGN KEY (to_account)   REFERENCES accounts(account_id),
    INDEX idx_txn_from (from_account, created_at),
    INDEX idx_txn_to   (to_account, created_at)
) ENGINE=InnoDB;

CREATE TABLE transaction_log (
    log_id     BIGINT AUTO_INCREMENT PRIMARY KEY,
    request_id CHAR(36) NOT NULL,
    status     ENUM('PENDING','COMMITTED','ROLLED_BACK') NOT NULL,
    reason     VARCHAR(255) NULL,
    thread_id  VARCHAR(32) NULL,
    attempt    TINYINT NOT NULL DEFAULT 1,
    logged_at  TIMESTAMP(3) DEFAULT CURRENT_TIMESTAMP(3),
    INDEX idx_log_req (request_id),
    INDEX idx_log_status (status, logged_at)
) ENGINE=InnoDB;
